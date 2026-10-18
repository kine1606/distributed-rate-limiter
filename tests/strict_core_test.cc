#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "core/strict_core.h"

namespace ratelimiter {
namespace {

// Answers every call with canned results and records what it was asked.
class FakeBackend final : public RedisBackend {
 public:
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override {
    calls.push_back({rule.name, std::string(key), requested, rule.limit, rule.refill_per_sec, std::string(request_id),
                     rule.overage, rule.overage_extra});
    done(reply);
  }
  void HGetAll(std::string key, HashCallback done) override {
    hash_keys.push_back(std::move(key));
    done(plan);
  }

  struct Call {
    std::string rule;
    std::string key;
    std::uint32_t requested;
    std::uint32_t limit;
    double refill;
    std::string request_id;
    Overage overage;
    std::uint32_t extra;
  };
  std::vector<Call> calls;
  ScriptResult reply;
  std::vector<std::string> hash_keys;
  HashResult plan;  // empty: the customer has no plan hash
};

RuleTable Rules() {
  Rule open{.name = "open", .limit = 10, .refill_per_sec = 1, .failure_policy = FailurePolicy::kFailOpen};
  Rule closed{.name = "closed", .limit = 10, .refill_per_sec = 1, .failure_policy = FailurePolicy::kFailClosed};
  Rule quota{.name = "quota", .algorithm = Algorithm::kQuota, .strict = true, .limit_from = PlanLimit::kDaily};
  Rule burst{.name = "burst", .strict = true, .limit_from = PlanLimit::kBurst};
  return {{"open", open}, {"closed", closed}, {"quota", quota}, {"burst", burst}};
}

class StrictCoreTest : public ::testing::Test {
 protected:
  CheckResult Run(std::string_view key, std::string_view rule, std::uint32_t cost = 1, std::string_view id = {}) {
    CheckResult out;
    bool called = false;
    core.Check({key, rule, cost, id}, [&](const CheckResult& r) {
      out = r;
      called = true;
    });
    EXPECT_TRUE(called);
    return out;
  }

  FakeBackend redis;
  PlanStore plans{redis};
  StrictCore core{redis, plans, Rules()};
};

TEST_F(StrictCoreTest, AllowedReplyPassesThroughWithLimit) {
  redis.reply = {.allowed = true, .remaining = 7};
  const auto r = Run("user:1", "open", 3);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kAllowed);
  EXPECT_EQ(r.remaining, 7u);
  EXPECT_EQ(r.limit, 10u);
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].key, "user:1");
  EXPECT_EQ(redis.calls[0].rule, "open");
  EXPECT_EQ(redis.calls[0].requested, 3u);
  EXPECT_TRUE(redis.hash_keys.empty());  // a rule with its own limit never reads a plan
}

TEST_F(StrictCoreTest, EmptyBucketIsRateLimitedWithRetryAfter) {
  redis.reply = {.allowed = false, .remaining = 0, .retry_after_ms = 400};
  const auto r = Run("user:1", "open");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_EQ(r.retry_after_ms, 400u);
}

TEST_F(StrictCoreTest, UnknownRuleNeverReachesRedis) {
  const auto r = Run("user:1", "no_such_rule");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnknownRule);
  EXPECT_TRUE(redis.calls.empty());
}

TEST_F(StrictCoreTest, RedisDownFailOpenAllows) {
  redis.reply = {.redis_error = true};
  const auto r = Run("user:1", "open");
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnverified);
}

TEST_F(StrictCoreTest, RedisDownFailClosedIsUnavailable) {
  redis.reply = {.redis_error = true};
  const auto r = Run("user:1", "closed");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);
}

TEST_F(StrictCoreTest, QuotaRuleUsesTheCustomersDailyLimit) {
  redis.plan.fields = {{"plan", "pay"}, {"daily", "2000"}};
  redis.reply = {.allowed = true, .remaining = 1999, .reset_after_ms = 3600};
  const auto r = Run("cust:42", "quota");
  ASSERT_EQ(redis.hash_keys.size(), 1u);
  EXPECT_EQ(redis.hash_keys[0], "{cust:42}:plan");
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].limit, 2000u);
  EXPECT_EQ(redis.calls[0].key, "cust:42");
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.limit, 2000u);
  EXPECT_EQ(r.remaining, 1999u);
  EXPECT_EQ(r.reset_after_ms, 3600u);
}

TEST_F(StrictCoreTest, CustomerWithoutPlanGetsFree) {  // step B1: a sign-up race is never a free pass
  redis.reply = {.allowed = true};
  Run("cust:new", "quota");
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].limit, 200u);
}

TEST_F(StrictCoreTest, ExhaustedQuotaSaysComeBackAtReset) {
  redis.reply = {.allowed = false, .remaining = 0, .retry_after_ms = 5000, .reset_after_ms = 5000};
  const auto r = Run("cust:42", "quota");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kQuotaExhausted);
  EXPECT_EQ(r.retry_after_ms, 5000u);
  EXPECT_EQ(r.reset_after_ms, 5000u);
}

TEST_F(StrictCoreTest, BurstRuleTakesCapacityAndRefillFromThePlan) {
  redis.plan.fields = {{"burst", "20"}, {"refill", "5"}};
  redis.reply = {.allowed = true};
  Run("cust:42", "burst");
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].limit, 20u);
  EXPECT_EQ(redis.calls[0].refill, 5.0);
}

TEST_F(StrictCoreTest, QuotaRuleCarriesThePlansOveragePolicyAndTheRequestId) {
  redis.plan.fields = {{"plan", "pay_cap"}, {"daily", "2000"}, {"overage", "cap"}, {"extra", "1000"}};
  redis.reply = {.allowed = true};
  Run("cust:42", "quota", 1, "retry-7");
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].overage, Overage::kCap);
  EXPECT_EQ(redis.calls[0].extra, 1000u);
  EXPECT_EQ(redis.calls[0].request_id, "retry-7");
}

TEST_F(StrictCoreTest, BilledOverageIsAllowedWithReasonOverage) {
  redis.reply = {.allowed = true, .remaining = 0, .reset_after_ms = 900, .overage = true};
  const auto r = Run("cust:42", "quota");
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kOverage);
  EXPECT_EQ(r.remaining, 0u);
}

TEST_F(StrictCoreTest, RetriedRequestIdIsMarkedReplayed) {
  redis.reply = {.allowed = true, .remaining = 5, .replayed = true};
  EXPECT_TRUE(Run("cust:42", "quota", 1, "retry-7").replayed);
  redis.reply = {.allowed = true, .remaining = 5};
  EXPECT_FALSE(Run("cust:42", "quota", 1, "retry-8").replayed);
}

// The Week 4 plan cache under the strict engine: one plan read serves every check within the TTL.
TEST_F(StrictCoreTest, WithAPlanCacheOnlyTheFirstCheckReadsThePlan) {
  PlanStore cached{redis, std::chrono::seconds(5)};
  StrictCore with_cache{redis, cached, Rules()};
  redis.plan.fields = {{"daily", "2000"}};
  redis.reply = {.allowed = true};
  for (int i = 0; i < 3; ++i) with_cache.Check({"cust:42", "quota", 1}, [](const CheckResult&) {});
  EXPECT_EQ(redis.hash_keys.size(), 1u);
  ASSERT_EQ(redis.calls.size(), 3u);
  EXPECT_EQ(redis.calls[2].limit, 2000u);
}

TEST_F(StrictCoreTest, PlanReadFailureAppliesTheFailurePolicyWithoutAScriptCall) {
  redis.plan.redis_error = true;
  const auto r = Run("cust:42", "quota");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);
  EXPECT_TRUE(redis.calls.empty());
}

}  // namespace
}  // namespace ratelimiter
