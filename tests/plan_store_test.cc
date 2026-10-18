// PlanStore's cache (Week 4): hits, TTL, single flight, invalidation racing a read, errors. A fake
// Redis holds each plan read until the test answers it, and a fake clock drives the TTL.
#include "plan/plan_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <string>
#include <vector>

namespace ratelimiter {
namespace {

std::int64_t g_now = 0;
std::int64_t FakeNow() { return g_now; }
constexpr std::int64_t kSec = 1'000'000'000;

class FakeRedis final : public RedisBackend {
 public:
  using RedisBackend::Eval;
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done({.redis_error = true});
  }
  void HGetAll(std::string key, HashCallback done) override {
    keys.push_back(key);
    pending.push_back(std::move(done));
  }
  // Answers the oldest read with a plan of `daily` a day, or an error.
  void Answer(const std::string& daily, bool error = false) {
    HashCallback done = std::move(pending.front());
    pending.erase(pending.begin());
    HashResult h;
    h.redis_error = error;
    if (!error) h.fields = {{"daily", daily}};
    done(h);
  }
  std::vector<std::string> keys;
  std::vector<HashCallback> pending;
};

class PlanStoreTest : public ::testing::Test {
 protected:
  void SetUp() override { g_now = 0; }

  // Starts a Get; the returned list fills when it is answered (daily, or 0 for nullptr).
  std::vector<std::uint32_t>& Get(const std::string& customer = "cust:1") {
    auto& out = answers.emplace_back();
    store.Get(customer, [&out](const Plan* p) { out.push_back(p ? p->daily : 0); });
    return out;
  }

  FakeRedis redis;
  PlanStore store{redis, std::chrono::seconds(5), &FakeNow};
  std::deque<std::vector<std::uint32_t>> answers;  // a deque: Get hands out references into it
};

TEST_F(PlanStoreTest, ZeroTtlReadsRedisOnEveryCall) {  // the Week 2 baseline
  PlanStore uncached(redis);
  for (int i = 0; i < 3; ++i) uncached.Get("cust:1", [](const Plan*) {});
  EXPECT_EQ(redis.pending.size(), 3u);
  EXPECT_EQ(redis.keys.front(), "{cust:1}:plan");
}

TEST_F(PlanStoreTest, HitAnswersInlineWithoutARead) {
  auto& first = Get();
  redis.Answer("2000");
  EXPECT_EQ(first, std::vector<std::uint32_t>{2000});
  g_now = 5 * kSec - 1;
  auto& second = Get();
  EXPECT_EQ(second, std::vector<std::uint32_t>{2000});  // answered before Get returned
  EXPECT_EQ(redis.keys.size(), 1u);
}

TEST_F(PlanStoreTest, ExpiredPlanIsReadAgain) {
  Get();
  redis.Answer("200");
  g_now = 5 * kSec;
  auto& later = Get();
  EXPECT_TRUE(later.empty());
  ASSERT_EQ(redis.keys.size(), 2u);
  redis.Answer("2000");  // upgraded meanwhile, message missed: the TTL bounds the delay
  EXPECT_EQ(later, std::vector<std::uint32_t>{2000});
}

TEST_F(PlanStoreTest, ConcurrentMissesShareOneRead) {
  auto& a = Get();
  auto& b = Get();
  auto& c = Get();
  auto& other = Get("cust:2");
  EXPECT_EQ(redis.keys, (std::vector<std::string>{"{cust:1}:plan", "{cust:2}:plan"}));
  redis.Answer("2000");
  EXPECT_EQ(a, std::vector<std::uint32_t>{2000});
  EXPECT_EQ(b, std::vector<std::uint32_t>{2000});
  EXPECT_EQ(c, std::vector<std::uint32_t>{2000});
  EXPECT_TRUE(other.empty());  // another customer, another read
}

TEST_F(PlanStoreTest, InvalidateMakesTheNextGetReadRedis) {  // step B5: the plan-change message
  Get();
  redis.Answer("200");
  store.Invalidate("cust:1");
  store.Invalidate("cust:never-seen");  // no-op
  auto& next = Get();
  ASSERT_EQ(redis.keys.size(), 2u);
  redis.Answer("2000");
  EXPECT_EQ(next, std::vector<std::uint32_t>{2000});
}

// The read may have seen the hash before rlctl rewrote it, so its answer is thrown away and every
// waiter, including one that joined after the message, gets a fresh read.
TEST_F(PlanStoreTest, InvalidateDuringAReadReadsAgainForEveryWaiter) {
  auto& before = Get();
  store.Invalidate("cust:1");
  auto& after = Get();
  redis.Answer("200");  // the stale read
  EXPECT_TRUE(before.empty());
  EXPECT_TRUE(after.empty());
  ASSERT_EQ(redis.pending.size(), 1u);  // read again
  redis.Answer("2000");
  EXPECT_EQ(before, std::vector<std::uint32_t>{2000});
  EXPECT_EQ(after, std::vector<std::uint32_t>{2000});
  Get();
  EXPECT_EQ(redis.keys.size(), 2u);  // and the fresh one is cached
}

TEST_F(PlanStoreTest, InvalidateAllDropsEveryPlanAndMarksReadsInFlight) {
  Get("cust:1");
  redis.Answer("200");
  auto& loading = Get("cust:2");
  store.InvalidateAll();  // the subscription was lost and is back
  redis.Answer("200");
  EXPECT_TRUE(loading.empty());
  redis.Answer("2000");
  EXPECT_EQ(loading, std::vector<std::uint32_t>{2000});
  Get("cust:1");
  EXPECT_EQ(redis.keys.size(), 4u);  // cust:1 read again
}

TEST_F(PlanStoreTest, ErrorsReachEveryWaiterAndAreNotCached) {
  auto& a = Get();
  auto& b = Get();
  redis.Answer("", true);
  EXPECT_EQ(a, std::vector<std::uint32_t>{0});
  EXPECT_EQ(b, std::vector<std::uint32_t>{0});
  Get();
  EXPECT_EQ(redis.keys.size(), 2u);  // the next check asks Redis again
}

TEST_F(PlanStoreTest, ACallbackMayCallGetAgain) {  // no lock is held while answering
  Get();
  redis.Answer("200");
  std::uint32_t inner = 0;
  store.Get("cust:1", [&](const Plan*) { store.Get("cust:1", [&](const Plan* p) { inner = p->daily; }); });
  EXPECT_EQ(inner, 200u);
}

TEST_F(PlanStoreTest, CustomersWhoStopCallingAreSweptAway) {
  for (int i = 0; i < 100'000; ++i) {
    store.Get("cust:" + std::to_string(i), [](const Plan*) {});
    redis.Answer("200");
    g_now += kSec;  // each plan has expired by the time the shard grows past its threshold
  }
  EXPECT_LT(store.Size(), 100'000u / 2);  // expired plans were dropped as the shards grew
  Get("cust:99999");
  EXPECT_EQ(redis.keys.size(), 100'000u);  // the newest plan is still cached
}

}  // namespace
}  // namespace ratelimiter
