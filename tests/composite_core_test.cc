// CompositeCore: one test per branch of the decision tree (src/core/README.md, Week 4), on a fake
// Redis that answers per rule and a fake clock.
#include "core/composite_core.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ratelimiter {
namespace {

std::int64_t g_now = 0;
std::int64_t FakeNow() { return g_now; }
constexpr std::int64_t kMs = 1'000'000;

class FakeBackend final : public RedisBackend {
 public:
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override {
    calls.push_back({rule.name, std::string(key), requested, std::string(request_id), rule.limit});
    done(replies[rule.name]);
  }
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    returns.push_back({rule.name, std::string(key), tokens, {}, rule.limit});
    done({.allowed = true});
  }
  void HGetAll(std::string, HashCallback done) override {
    ++plan_reads;
    done(plan);
  }

  struct Call {
    std::string rule;
    std::string key;
    std::uint32_t amount;
    std::string request_id;
    std::uint32_t limit;
  };
  std::vector<Call> calls, returns;
  std::unordered_map<std::string, ScriptResult> replies;  // by rule name; default: denied
  HashResult plan;
  int plan_reads = 0;
};

// Records what reaches the engine behind CompositeCore.
class FakeInner final : public RateLimiterCore {
 public:
  void Check(const CheckRequest& request, CheckCallback done) override {
    rules.emplace_back(request.rule);
    done({.allowed = true});
  }
  std::vector<std::string> rules;
};

RuleTable Rules() {
  Rule burst{.name = "burst", .strict = true, .limit_from = PlanLimit::kBurst, .failure_policy = FailurePolicy::kFailOpen};
  Rule quota{.name = "daily_quota", .algorithm = Algorithm::kQuota, .strict = true, .limit_from = PlanLimit::kDaily};
  Rule customer{.name = "customer", .burst_rule = "burst", .quota_rule = "daily_quota"};
  Rule other{.name = "api_default", .limit = 1000, .refill_per_sec = 500};
  return {{"burst", burst}, {"daily_quota", quota}, {"customer", customer}, {"api_default", other}};
}

class CompositeCoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_now = 0;
    redis.plan.fields = {{"plan", "pay"}, {"daily", "2000"}, {"burst", "20"}, {"refill", "20"}};
    redis.replies["burst"] = {.allowed = true, .remaining = 19};
    redis.replies["daily_quota"] = {.allowed = true, .remaining = 1999, .reset_after_ms = 60'000};
  }

  CheckResult Run(std::string_view rule = "customer", std::uint32_t cost = 1, std::string_view id = {}) {
    CheckResult out;
    bool called = false;
    core.Check({"cust:42", rule, cost, id}, [&](const CheckResult& r) {
      out = r;
      called = true;
    });
    EXPECT_TRUE(called);
    return out;
  }

  void ExhaustQuota() {
    redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000};
    ASSERT_EQ(Run().reason, Reason::kQuotaExhausted);
    redis.calls.clear();
    redis.returns.clear();
  }

  FakeBackend redis;
  PlanStore plans{redis};
  StrictCore strict{redis, plans, Rules()};
  DenyCache deny;
  std::shared_ptr<FakeInner> inner = std::make_shared<FakeInner>();
  CompositeCore core{inner, strict, plans, deny, Rules(), &FakeNow};
};

TEST_F(CompositeCoreTest, OtherRulesGoToTheInnerEngineUntouched) {
  Run("api_default");
  Run("no_such_rule");
  EXPECT_EQ(inner->rules, (std::vector<std::string>{"api_default", "no_such_rule"}));
  EXPECT_EQ(redis.plan_reads, 0);
}

TEST_F(CompositeCoreTest, AllowedAnswersWithTheDailyQuotaAfterOnePlanRead) {
  const auto r = Run("customer", 3);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kAllowed);
  EXPECT_EQ(r.limit, 2000u);
  EXPECT_EQ(r.remaining, 1999u);
  EXPECT_EQ(r.reset_after_ms, 60'000u);
  EXPECT_EQ(redis.plan_reads, 1);  // one read serves both rules
  ASSERT_EQ(redis.calls.size(), 2u);
  EXPECT_EQ(redis.calls[0].rule, "burst");  // burst first, with the plan's numbers
  EXPECT_EQ(redis.calls[0].limit, 20u);
  EXPECT_EQ(redis.calls[0].amount, 3u);
  EXPECT_EQ(redis.calls[1].rule, "daily_quota");
  EXPECT_EQ(redis.calls[1].limit, 2000u);
  EXPECT_TRUE(inner->rules.empty());
}

TEST_F(CompositeCoreTest, SuspendedIsDeniedWithoutAScriptCall) {
  redis.plan.fields = {{"plan", "suspended"}, {"daily", "0"}};
  const auto r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kSuspended);
  EXPECT_EQ(r.retry_after_ms, 0u);  // waiting does not help
  EXPECT_TRUE(redis.calls.empty());
}

TEST_F(CompositeCoreTest, BurstDenialAnswersAtOnceAndUsesNoQuota) {
  redis.replies["burst"] = {.allowed = false, .retry_after_ms = 500};
  const auto r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_EQ(r.retry_after_ms, 500u);
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].rule, "burst");
}

TEST_F(CompositeCoreTest, CapOverageIsAllowedAsOverage) {
  redis.replies["daily_quota"] = {.allowed = true, .remaining = 0, .reset_after_ms = 60'000, .overage = true};
  const auto r = Run();
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kOverage);
  EXPECT_TRUE(redis.returns.empty());
}

// The bug the README warns about: without the refund an over-quota customer drains their own burst
// bucket and later sees RATE_LIMITED instead of the true reason.
TEST_F(CompositeCoreTest, QuotaDenialGivesTheBurstTokenBack) {
  redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000};
  const auto r = Run("customer", 2);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kQuotaExhausted);
  EXPECT_EQ(r.retry_after_ms, 60'000u);
  ASSERT_EQ(redis.returns.size(), 1u);
  EXPECT_EQ(redis.returns[0].rule, "burst");
  EXPECT_EQ(redis.returns[0].key, "cust:42");
  EXPECT_EQ(redis.returns[0].amount, 2u);
  EXPECT_EQ(redis.returns[0].limit, 20u);  // the plan's capacity bounds the refund
}

TEST_F(CompositeCoreTest, DeniedCustomerIsAnsweredLocallyUntilTheReset) {
  ExhaustQuota();
  g_now = 20'000 * kMs;
  const auto r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kQuotaExhausted);
  EXPECT_EQ(r.retry_after_ms, 40'000u);
  EXPECT_EQ(r.reset_after_ms, 40'000u);
  EXPECT_EQ(r.limit, 2000u);
  EXPECT_TRUE(redis.calls.empty());  // no script call: neither burst nor quota
  EXPECT_TRUE(redis.returns.empty());

  g_now = 60'000 * kMs;  // the new UTC day: Redis decides again
  Run();
  EXPECT_EQ(redis.calls.size(), 2u);
}

TEST_F(CompositeCoreTest, UpgradeLiftsTheLocalDenialAtOnce) {  // step B5
  ExhaustQuota();
  redis.plan.fields["daily"] = "20000";
  redis.replies["daily_quota"] = {.allowed = true, .remaining = 17999};
  const auto r = Run();
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.limit, 20000u);
}

TEST_F(CompositeCoreTest, RequestIdGoesToTheQuotaOnly) {  // burst is not deduplicated
  Run("customer", 1, "retry-1");
  ASSERT_EQ(redis.calls.size(), 2u);
  EXPECT_EQ(redis.calls[0].request_id, "");
  EXPECT_EQ(redis.calls[1].request_id, "retry-1");
}

TEST_F(CompositeCoreTest, PlanReadFailureAppliesTheQuotasFailurePolicy) {
  redis.plan.redis_error = true;
  const auto r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);  // daily_quota fails closed
  EXPECT_TRUE(redis.calls.empty());
}

TEST_F(CompositeCoreTest, QuotaUnreachableDeniesAndCachesNothing) {
  redis.replies["daily_quota"] = {.redis_error = true};
  EXPECT_EQ(Run().reason, Reason::kUnavailable);
  EXPECT_EQ(redis.returns.size(), 1u);  // the request was denied, so its burst token goes back
  EXPECT_EQ(deny.Size(), 0u);           // not a quota decision: nothing to remember
}

TEST_F(CompositeCoreTest, UncountedBurstIsNotGivenBack) {
  redis.replies["burst"] = {.redis_error = true};  // burst fails open: allowed, unverified, not counted
  redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000};
  EXPECT_EQ(Run().reason, Reason::kQuotaExhausted);
  EXPECT_TRUE(redis.returns.empty());
}

// The original was denied under the old plan; its replay after an upgrade must not deny the
// customer locally under the new one.
TEST_F(CompositeCoreTest, AReplayedDenialIsNotCached) {
  redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000,
                                  .replayed = true};
  EXPECT_EQ(Run("customer", 1, "retry-1").reason, Reason::kQuotaExhausted);
  EXPECT_EQ(deny.Size(), 0u);
  EXPECT_EQ(redis.returns.size(), 1u);  // the retry's own burst token still goes back
}

struct Decision {
  std::string customer;
  Reason reason;
  std::uint32_t cost;
  bool operator==(const Decision&) const = default;
};

// Usage events (Week 4): every customer decision is recorded once, with its reason and cost.
TEST_F(CompositeCoreTest, EveryCustomerDecisionIsRecordedOnce) {
  std::vector<Decision> seen;
  CompositeCore recorded{inner, strict, plans, deny, Rules(), &FakeNow,
                         [&](std::string_view c, Reason r, std::uint32_t cost) {
                           seen.push_back({std::string(c), r, cost});
                         }};
  const auto run = [&](std::uint32_t cost) {
    recorded.Check({"cust:42", "customer", cost}, [](const CheckResult&) {});
  };
  run(3);                                                       // allowed
  redis.replies["burst"] = {.allowed = false, .retry_after_ms = 500};
  run(1);                                                       // burst denial
  redis.replies["burst"] = {.allowed = true};
  redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000};
  run(1);                                                       // quota denial
  run(1);                                                       // deny cache
  redis.plan.fields["plan"] = "suspended";
  run(2);                                                       // suspended
  recorded.Check({"cust:42", "api_default", 1}, [](const CheckResult&) {});  // not a customer rule
  EXPECT_EQ(seen, (std::vector<Decision>{{"cust:42", Reason::kAllowed, 3},
                                         {"cust:42", Reason::kRateLimited, 1},
                                         {"cust:42", Reason::kQuotaExhausted, 1},
                                         {"cust:42", Reason::kQuotaExhausted, 1},
                                         {"cust:42", Reason::kSuspended, 2}}));
}

// A retry's stored answer was recorded with the original request, so billing counts it once.
TEST_F(CompositeCoreTest, ARetriedRequestIdIsNotRecordedAgain) {
  int events = 0;
  CompositeCore recorded{inner, strict, plans, deny, Rules(), &FakeNow,
                         [&](std::string_view, Reason, std::uint32_t) { ++events; }};
  redis.replies["daily_quota"] = {.allowed = true, .remaining = 1999, .replayed = true};
  CheckResult r;
  recorded.Check({"cust:42", "customer", 1, "retry-1"}, [&](const CheckResult& x) { r = x; });
  EXPECT_TRUE(r.allowed);
  EXPECT_TRUE(r.replayed);
  EXPECT_EQ(events, 0);
}

}  // namespace
}  // namespace ratelimiter
