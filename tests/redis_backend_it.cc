// RedisPPBackend against the dev Redis Cluster (docker/docker-compose.dev.yml). Seed node: env
// RL_TEST_CLUSTER="host:port" (default 127.0.0.1:7001). Skipped, not failed, when unreachable.
#include <gtest/gtest.h>
#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include "it_cluster.h"
#include "redis/keys.h"
#include "redis/redispp_backend.h"

namespace ratelimiter {
namespace {

class RedisBackendIT : public ::testing::Test {
 protected:
  void SetUp() override {
    admin_ = ConnectAdmin();
    if (!admin_) GTEST_SKIP() << "Redis Cluster not reachable at " << ClusterSeed().first << ":" << ClusterSeed().second;
    backend_ = std::make_unique<RedisPPBackend>(ClusterSeed().first, ClusterSeed().second,
                                                std::string(RL_SOURCE_DIR) + "/lua");
  }

  static ScriptResult EvalSync(RedisBackend& b, const Rule& rule, std::string_view key) {
    std::promise<ScriptResult> p;
    b.Eval(rule, key, 1, [&p](const ScriptResult& r) { p.set_value(r); });
    return p.get_future().get();
  }

  void FlushScriptsOnEveryMaster() {
    admin_->for_each([](sw::redis::Redis& master) { master.command("SCRIPT", "FLUSH"); });
  }

  std::unique_ptr<sw::redis::RedisCluster> admin_;
  std::unique_ptr<RedisPPBackend> backend_;
};

// The same budget (5) under each algorithm. Windows are an hour long so no window boundary falls
// inside the test.
const Rule kAllAlgorithms[] = {
    {.name = "it_tb", .limit = 5, .refill_per_sec = 1},
    {.name = "it_sw", .algorithm = Algorithm::kSlidingWindow, .limit = 5, .window_ms = 3'600'000},
    {.name = "it_fw", .algorithm = Algorithm::kFixedWindow, .limit = 5, .window_ms = 3'600'000},
    {.name = "it_q", .algorithm = Algorithm::kQuota, .limit = 5},
};

TEST_F(RedisBackendIT, EveryAlgorithmAdmitsExactlyTheLimitThenDenies) {
  for (const Rule& rule : kAllAlgorithms) {
    SCOPED_TRACE(rule.name);
    admin_->del(RedisKey(rule, "it:b1"));
    for (int i = 4; i >= 0; --i) {
      const auto r = EvalSync(*backend_, rule, "it:b1");
      ASSERT_FALSE(r.redis_error);
      EXPECT_TRUE(r.allowed);
      EXPECT_EQ(r.remaining, static_cast<std::uint32_t>(i));
    }
    const auto d = EvalSync(*backend_, rule, "it:b1");
    ASSERT_FALSE(d.redis_error);
    EXPECT_FALSE(d.allowed);
    EXPECT_GT(d.retry_after_ms, 0u);
  }
}

TEST_F(RedisBackendIT, QuotaDenialRetriesAtTheDailyReset) {
  const Rule& quota = kAllAlgorithms[3];
  admin_->del(RedisKey(quota, "it:b2"));
  const auto first = EvalSync(*backend_, quota, "it:b2");
  EXPECT_GT(first.reset_after_ms, 0u);
  EXPECT_LE(first.reset_after_ms, 86'400'000u);
  EXPECT_EQ(first.retry_after_ms, 0u);
  for (int i = 0; i < 4; ++i) EvalSync(*backend_, quota, "it:b2");
  const auto d = EvalSync(*backend_, quota, "it:b2");
  EXPECT_FALSE(d.allowed);
  EXPECT_EQ(d.retry_after_ms, d.reset_after_ms);
}

// Calls fired without waiting share each connection's pipeline; every reply must reach its own
// caller. Two keys with different limits, interleaved: a reply delivered to the wrong caller shows
// up as a wrong admitted count.
TEST_F(RedisBackendIT, PipelinedRepliesReachTheirOwnCallers) {
  const Rule a{.name = "it_pipe_a", .limit = 300, .refill_per_sec = 0.001};
  const Rule b{.name = "it_pipe_b", .limit = 700, .refill_per_sec = 0.001};
  admin_->del(RedisKey(a, "it:hp"));
  admin_->del(RedisKey(b, "it:hp"));

  std::atomic<int> allowed_a{0}, allowed_b{0}, errors{0}, pending{2000};
  std::promise<void> all_done;
  auto count = [&](std::atomic<int>& allowed) {
    return [&](const ScriptResult& r) {
      if (r.redis_error) ++errors;
      if (r.allowed) ++allowed;
      if (--pending == 0) all_done.set_value();
    };
  };
  for (int i = 0; i < 1000; ++i) {
    backend_->Eval(a, "it:hp", 1, count(allowed_a));
    backend_->Eval(b, "it:hp", 1, count(allowed_b));
  }
  all_done.get_future().get();
  EXPECT_EQ(errors.load(), 0);
  EXPECT_EQ(allowed_a.load(), 300);
  EXPECT_EQ(allowed_b.load(), 700);
}

// 1,000 quota calls for 100 customers spread over all masters, with every script cache flushed in
// the middle: NOSCRIPT must fall back to EVAL, and each call must count exactly once.
TEST_F(RedisBackendIT, QuotaCountsExactlyAcrossTheClusterWhileScriptsAreFlushed) {
  const Rule quota{.name = "it_count", .algorithm = Algorithm::kQuota, .limit = 1000};
  const auto customer = [](int i) { return "it:c" + std::to_string(i); };
  for (int i = 0; i < 100; ++i) admin_->del(RedisKey(quota, customer(i)));

  std::atomic<int> errors{0}, pending{1000};
  std::promise<void> all_done;
  const auto fire_round = [&] {  // one call per customer
    for (int i = 0; i < 100; ++i) {
      backend_->Eval(quota, customer(i), 1, [&](const ScriptResult& r) {
        if (r.redis_error || !r.allowed) ++errors;
        if (--pending == 0) all_done.set_value();
      });
    }
  };
  for (int round = 0; round < 5; ++round) fire_round();
  FlushScriptsOnEveryMaster();
  for (int round = 0; round < 5; ++round) fire_round();
  all_done.get_future().get();

  EXPECT_EQ(errors.load(), 0);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(admin_->hget(RedisKey(quota, customer(i)), "used").value_or("missing"), "10") << customer(i);
  }
}

TEST_F(RedisBackendIT, HGetAllReadsTheHashAndAMissingKeyIsEmpty) {
  admin_->del("{it:h}:plan");
  admin_->hset("{it:h}:plan", "daily", "2000");
  std::promise<HashResult> p, missing;
  backend_->HGetAll("{it:h}:plan", [&p](const HashResult& r) { p.set_value(r); });
  backend_->HGetAll("{it:none}:plan", [&missing](const HashResult& r) { missing.set_value(r); });
  const auto r = p.get_future().get();
  ASSERT_FALSE(r.redis_error);
  EXPECT_EQ(r.fields.at("daily"), "2000");
  const auto m = missing.get_future().get();
  EXPECT_FALSE(m.redis_error);
  EXPECT_TRUE(m.fields.empty());
}

// A dead cluster must surface as redis_error (the engine applies the failure policy), and never
// park the calling gRPC thread.
TEST(RedisBackendDown, UnreachableClusterReportsErrorsWithoutBlocking) {
  RedisPPBackend backend("127.0.0.1", 1, std::string(RL_SOURCE_DIR) + "/lua");
  for (int i = 0; i < 5; ++i) {
    std::promise<ScriptResult> p;
    const auto start = std::chrono::steady_clock::now();
    backend.Eval(kAllAlgorithms[0], "x", 1, [&p](const ScriptResult& r) { p.set_value(r); });
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(150)) << "call " << i;
    EXPECT_TRUE(p.get_future().get().redis_error);
  }
  std::promise<HashResult> h;
  backend.HGetAll("{x}:plan", [&h](const HashResult& r) { h.set_value(r); });
  EXPECT_TRUE(h.get_future().get().redis_error);
}

}  // namespace
}  // namespace ratelimiter
