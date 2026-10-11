// Lua script tests against a REAL Redis (no mocks: the whole point is Redis semantics).
// Redis address: env RL_TEST_REDIS="host:port" (default 127.0.0.1:6379).
// If Redis is unreachable the tests are skipped, not failed.
#include <hiredis/hiredis.h>
#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Reply {
  std::vector<long long> ints;
  bool error = false;
  std::string err;
};

class LuaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string host = "127.0.0.1";
    int port = 6379;
    if (const char* e = std::getenv("RL_TEST_REDIS")) {
      std::string a = e;
      auto c = a.find(':');
      host = a.substr(0, c);
      if (c != std::string::npos) port = std::stoi(a.substr(c + 1));
    }
    ctx_.reset(redisConnect(host.c_str(), port));
    if (!ctx_ || ctx_->err) GTEST_SKIP() << "Redis not reachable at " << host << ":" << port;
    tb_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/token_bucket.lua");
    quota_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/quota.lua");
  }

  std::string Load(const std::string& path) {
    std::string src = ReadFile(path);
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "SCRIPT LOAD %b", src.data(), src.size()));
    EXPECT_TRUE(r && r->type == REDIS_REPLY_STRING) << path;
    std::string sha = r ? r->str : "";
    freeReplyObject(r);
    return sha;
  }

  Reply Run(const std::string& sha, const std::string& key, const std::vector<std::string>& args) {
    std::vector<const char*> argv{"EVALSHA", sha.c_str(), "1", key.c_str()};
    for (auto& a : args) argv.push_back(a.c_str());
    auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_.get(), argv.size(), argv.data(), nullptr));
    Reply out;
    if (!r || r->type == REDIS_REPLY_ERROR) {
      out.error = true;
      out.err = r ? r->str : "null";
    } else {
      for (size_t i = 0; i < r->elements; ++i) out.ints.push_back(r->element[i]->integer);
    }
    if (r) freeReplyObject(r);
    return out;
  }

  long long Pttl(const std::string& key) {
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "PTTL %s", key.c_str()));
    long long v = r->integer;
    freeReplyObject(r);
    return v;
  }

  void Del(const std::string& key) { freeReplyObject(redisCommand(ctx_.get(), "DEL %s", key.c_str())); }

  // Token bucket: capacity, refill/s, requested, ttl_ms
  Reply TB(const std::string& key, int cap, int rate, int req, int ttl = 60000) {
    return Run(tb_sha_, key, {std::to_string(cap), std::to_string(rate), std::to_string(req), std::to_string(ttl)});
  }
  Reply Quota(const std::string& key, int limit, int cost) {
    return Run(quota_sha_, key, {std::to_string(limit), std::to_string(cost)});
  }

  struct Closer { void operator()(redisContext* c) const { redisFree(c); } };
  std::unique_ptr<redisContext, Closer> ctx_;
  std::string tb_sha_, quota_sha_;
};

// ---- token bucket ----

TEST_F(LuaTest, BucketAdmitsBurstThenDenies) {
  const std::string k = "{t1}:tb";
  Del(k);
  for (int i = 4; i >= 0; --i) {
    auto r = TB(k, 5, 1, 1);
    ASSERT_FALSE(r.error) << r.err;
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
  }
  auto d = TB(k, 5, 1, 1);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_GT(d.ints[2], 0);       // told when to retry
  EXPECT_LE(d.ints[2], 1000);    // at 1 token/s, at most one second
}

TEST_F(LuaTest, CostLargerThanBurstIsNeverAllowed) {
  const std::string k = "{t2}:tb";
  Del(k);
  auto r = TB(k, 5, 1, 10);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[2], 0);       // waiting would not help
  EXPECT_EQ(r.ints[1], 5);       // bucket untouched
}

TEST_F(LuaTest, RequestedChunkTakesManyTokensAtOnce) {
  const std::string k = "{t3}:tb";
  Del(k);
  auto r = TB(k, 100, 10, 40);   // a lease refill of 40
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 60);
}

TEST_F(LuaTest, RefillsOverTime) {
  const std::string k = "{t4}:tb";
  Del(k);
  TB(k, 10, 10, 10);             // drain
  EXPECT_EQ(TB(k, 10, 10, 1).ints[0], 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(350));  // ~3 tokens
  auto r = TB(k, 10, 10, 2);
  EXPECT_EQ(r.ints[0], 1);
}

TEST_F(LuaTest, ClockJumpBackwardsNeverCreatesTokens) {
  const std::string k = "{t5}:tb";
  Del(k);
  TB(k, 5, 1, 5);                // drain
  // Simulate the stored timestamp being far in the future (clock moved back).
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s last_refill_ms 99999999999999", k.c_str()));
  auto r = TB(k, 5, 1, 1);
  EXPECT_EQ(r.ints[0], 0);       // elapsed clamped to 0, no free tokens
}

TEST_F(LuaTest, IdleKeysCarryTtlAndExpire) {
  const std::string k = "{t6}:tb";
  Del(k);
  TB(k, 5, 1, 1, 200);
  long long ttl = Pttl(k);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(Pttl(k), -2);        // key is gone
  EXPECT_EQ(TB(k, 5, 1, 1).ints[1], 4);  // and a new bucket starts full
}

TEST_F(LuaTest, ConcurrentCallersNeverOverAdmit) {
  const std::string k = "{t7}:tb";
  Del(k);
  std::atomic<int> admitted{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&] {
      redisContext* c = redisConnect("127.0.0.1", 6379);
      if (const char* e = std::getenv("RL_TEST_REDIS")) {
        redisFree(c);
        std::string a = e;
        auto p = a.find(':');
        c = redisConnect(a.substr(0, p).c_str(), std::stoi(a.substr(p + 1)));
      }
      for (int i = 0; i < 50; ++i) {
        auto* r = static_cast<redisReply*>(redisCommand(c, "EVALSHA %s 1 %s 100 0 1 60000", tb_sha_.c_str(), k.c_str()));
        if (r && r->type == REDIS_REPLY_ARRAY && r->element[0]->integer == 1) ++admitted;
        if (r) freeReplyObject(r);
      }
      redisFree(c);
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(admitted.load(), 100);   // 400 attempts, rate 0, capacity 100 => exactly 100
}

// ---- daily quota ----

TEST_F(LuaTest, QuotaCountsUpToLimit) {
  const std::string k = "{q1}:quota";
  Del(k);
  for (int i = 1; i <= 3; ++i) {
    auto r = Quota(k, 3, 1);
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
    EXPECT_EQ(r.ints[2], 3 - i);
  }
  auto d = Quota(k, 3, 1);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_EQ(d.ints[1], 3);       // a denied request is not counted
  EXPECT_GT(d.ints[3], 0);
  EXPECT_LE(d.ints[3], 86400000);
}

TEST_F(LuaTest, QuotaUpgradeMidDayKeepsUsageAndAppliesImmediately) {
  const std::string k = "{q2}:quota";
  Del(k);
  for (int i = 0; i < 3; ++i) Quota(k, 3, 1);
  EXPECT_EQ(Quota(k, 3, 1).ints[0], 0);
  auto r = Quota(k, 10, 1);      // customer upgraded
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 4);
  EXPECT_EQ(r.ints[2], 6);
}

TEST_F(LuaTest, QuotaDowngradeBelowUsageDeniesWithoutNegativeRemaining) {
  const std::string k = "{q3}:quota";
  Del(k);
  for (int i = 0; i < 5; ++i) Quota(k, 10, 1);
  auto r = Quota(k, 2, 1);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[2], 0);
}

TEST_F(LuaTest, QuotaCostGreaterThanRemainingIsDeniedAndNotCounted) {
  const std::string k = "{q4}:quota";
  Del(k);
  Quota(k, 5, 3);
  auto r = Quota(k, 5, 3);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[1], 3);
}

TEST_F(LuaTest, QuotaResetsOnNewUtcDay) {
  const std::string k = "{q5}:quota";
  Del(k);
  for (int i = 0; i < 3; ++i) Quota(k, 3, 1);
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s day 1", k.c_str()));  // pretend it is yesterday's data
  auto r = Quota(k, 3, 1);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 1);
}

TEST_F(LuaTest, QuotaKeyExpiresAfterDayEnds) {
  const std::string k = "{q6}:quota";
  Del(k);
  Quota(k, 3, 1);
  long long ttl = Pttl(k);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 86400000 + 3600000);
}

}  // namespace
