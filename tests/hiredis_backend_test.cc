// HiredisBackend against a REAL Redis. Address: env RL_TEST_REDIS="host:port"
// (default 127.0.0.1:6379). Skipped, not failed, when Redis is unreachable.
#include <hiredis/hiredis.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <future>
#include <string>

#include "redis/hiredis_backend.h"

namespace ratelimiter {
namespace {

class HiredisBackendTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (const char* e = std::getenv("RL_TEST_REDIS")) {
      std::string a = e;
      const auto c = a.find(':');
      host_ = a.substr(0, c);
      if (c != std::string::npos) port_ = std::stoi(a.substr(c + 1));
    }
    admin_ = redisConnect(host_.c_str(), port_);
    if (!admin_ || admin_->err) GTEST_SKIP() << "Redis not reachable at " << host_ << ":" << port_;
    backend_ = std::make_unique<HiredisBackend>(host_, port_, std::string(RL_SOURCE_DIR) + "/lua");
  }

  void TearDown() override {
    backend_.reset();
    if (admin_) redisFree(admin_);
  }

  void Admin(const std::string& cmd) { freeReplyObject(redisCommand(admin_, cmd.c_str())); }

  static ScriptResult EvalSync(RedisBackend& b, const Rule& rule, std::string_view key) {
    std::promise<ScriptResult> p;
    b.Eval(rule, key, 1, [&p](const ScriptResult& r) { p.set_value(r); });
    return p.get_future().get();
  }

  std::string host_ = "127.0.0.1";
  int port_ = 6379;
  redisContext* admin_ = nullptr;
  std::unique_ptr<HiredisBackend> backend_;
};

const Rule kFive{.name = "it_five", .limit = 5, .refill_per_sec = 1};

// The same budget (5) under each algorithm. Windows are an hour long so no window boundary
// falls inside the test.
const Rule kAllAlgorithms[] = {
    kFive,
    {.name = "it_sw", .algorithm = Algorithm::kSlidingWindow, .limit = 5, .window_ms = 3'600'000},
    {.name = "it_fw", .algorithm = Algorithm::kFixedWindow, .limit = 5, .window_ms = 3'600'000},
};

TEST_F(HiredisBackendTest, EveryAlgorithmAdmitsExactlyTheLimitThenDenies) {
  for (const Rule& rule : kAllAlgorithms) {
    SCOPED_TRACE(rule.name);
    Admin("DEL " + RedisKey(rule, "hb1"));
    for (int i = 4; i >= 0; --i) {
      const auto r = EvalSync(*backend_, rule, "hb1");
      ASSERT_FALSE(r.redis_error);
      EXPECT_TRUE(r.allowed);
      EXPECT_EQ(r.remaining, static_cast<std::uint32_t>(i));
    }
    const auto d = EvalSync(*backend_, rule, "hb1");
    ASSERT_FALSE(d.redis_error);
    EXPECT_FALSE(d.allowed);
    EXPECT_GT(d.retry_after_ms, 0u);
  }
}

TEST_F(HiredisBackendTest, KeysOfDifferentUsersAreIndependent) {
  Admin("DEL " + RedisKey(kFive, "hb2a"));
  Admin("DEL " + RedisKey(kFive, "hb2b"));
  for (int i = 0; i < 5; ++i) EvalSync(*backend_, kFive, "hb2a");
  EXPECT_FALSE(EvalSync(*backend_, kFive, "hb2a").allowed);
  EXPECT_TRUE(EvalSync(*backend_, kFive, "hb2b").allowed);
}

TEST_F(HiredisBackendTest, RecoversWhenScriptCacheIsFlushed) {
  Admin("DEL " + RedisKey(kFive, "hb3"));
  ASSERT_FALSE(EvalSync(*backend_, kFive, "hb3").redis_error);
  Admin("SCRIPT FLUSH");  // EVALSHA now gets NOSCRIPT; the backend must fall back to EVAL
  const auto r = EvalSync(*backend_, kFive, "hb3");
  EXPECT_FALSE(r.redis_error);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.remaining, 3u);
}

TEST(HiredisBackendDown, UnreachableRedisReportsErrorNotDenial) {
  HiredisBackend backend("127.0.0.1", 1, std::string(RL_SOURCE_DIR) + "/lua");
  std::promise<ScriptResult> p;
  backend.Eval(kFive, "x", 1, [&p](const ScriptResult& r) { p.set_value(r); });
  EXPECT_TRUE(p.get_future().get().redis_error);
}

TEST(RedisKey, HashTagIsTheLimitedEntityAndSuffixIsTheAlgorithm) {
  EXPECT_EQ(RedisKey(kFive, "user:42"), "{user:42}:it_five:tb");
  EXPECT_EQ(RedisKey(kAllAlgorithms[1], "user:42"), "{user:42}:it_sw:sw");
  EXPECT_EQ(RedisKey(kAllAlgorithms[2], "user:42"), "{user:42}:it_fw:fw");
}

}  // namespace
}  // namespace ratelimiter
