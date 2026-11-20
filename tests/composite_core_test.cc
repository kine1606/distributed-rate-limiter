// CompositeCore: one test per branch of the decision tree (src/core/README.md, Week 4), on a fake
// Redis that answers per rule and a fake clock.
#include "core/composite_core.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <string>
#include <tuple>
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
    calls.push_back({rule.name, std::string(key), requested, std::string(request_id), rule.limit, rule.overage});
    auto& queued = next[rule.name];
    if (!queued.empty()) {
      const ScriptResult r = queued.front();
      queued.pop_front();
      done(r);
      return;
    }
    done(replies[rule.name]);
  }
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    returns.push_back({rule.name, std::string(key), tokens, {}, rule.limit, rule.overage});
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
    Overage overage;
  };
  std::vector<Call> calls, returns;
  std::unordered_map<std::string, std::deque<ScriptResult>> next;  // by rule name, one per call, first
  std::unordered_map<std::string, ScriptResult> replies;            // by rule name; default: denied
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

TEST_F(CompositeCoreTest, AnUnknownCustomerWhileRedisIsDownIsFreeAndFailsClosed) {
  redis.plan.redis_error = true;
  const auto r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);
  EXPECT_EQ(r.limit, 200u);  // the Free plan
  EXPECT_TRUE(redis.calls.empty());
}

TEST_F(CompositeCoreTest, QuotaUnreachableDeniesAndCachesNothing) {
  redis.replies["daily_quota"] = {.redis_error = true};
  EXPECT_EQ(Run().reason, Reason::kUnavailable);
  EXPECT_EQ(redis.returns.size(), 1u);  // the request was denied, so its burst token goes back
  EXPECT_EQ(deny.Size(), 0u);           // not a quota decision: nothing to remember
}

// Week 5: when Redis cannot answer, the customer's plan decides (paid open, Free closed), whatever
// the rule's own failure_policy says.
TEST_F(CompositeCoreTest, RedisDownAtTheBurstStepFollowsThePlan) {
  redis.replies["burst"] = {.redis_error = true};
  redis.plan.fields["failure"] = "open";
  auto r = Run();
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnverified);  // admitted, not counted, not billed
  EXPECT_EQ(r.limit, 2000u);
  redis.plan.fields["failure"] = "closed";
  r = Run();
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);
  EXPECT_EQ(r.retry_after_ms, 1000u);
  for (const auto& c : redis.calls) EXPECT_EQ(c.rule, "burst");  // no quota call after a failed burst
  EXPECT_TRUE(redis.returns.empty());
}

TEST_F(CompositeCoreTest, RedisDownAtTheQuotaStepFollowsThePlan) {
  redis.replies["daily_quota"] = {.redis_error = true};
  redis.plan.fields["failure"] = "open";
  auto r = Run();
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnverified);
  EXPECT_TRUE(redis.returns.empty());  // admitted: its burst token stays taken
  redis.plan.fields.erase("failure");  // the Free default: closed
  r = Run();
  EXPECT_EQ(r.reason, Reason::kUnavailable);
  EXPECT_EQ(redis.returns.size(), 1u);  // denied: the burst token goes back
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

// ---- leased quota (Week 4): quota_mode: auto, plans of kLeasedQuotaFrom a day or more ----

RuleTable AutoRules() {
  RuleTable r = Rules();
  r.at("daily_quota").quota_auto = true;
  return r;
}

class LeasedQuotaTest : public CompositeCoreTest {
 protected:
  void SetUp() override {
    CompositeCoreTest::SetUp();
    redis.plan.fields = {{"plan", "enterprise"}, {"daily", "1000000"}, {"burst", "2000"}, {"refill", "2000"},
                         {"overage", "cap"},     {"extra", "100000"}};
    redis.replies["daily_quota"] = {.allowed = true, .remaining = 999'000, .reset_after_ms = 60'000};
  }
  CheckResult RunOn(RateLimiterCore& c, std::string_view id = {}) {
    CheckResult out;
    c.Check({"cust:ent", "customer", 1, id}, [&](const CheckResult& r) { out = r; });
    return out;
  }
  std::vector<FakeBackend::Call> QuotaCalls() const {
    std::vector<FakeBackend::Call> out;
    for (const auto& c : redis.calls) {
      if (c.rule == "daily_quota") out.push_back(c);
    }
    return out;
  }
  LeasedCore leased{redis, plans, AutoRules(), 1024, &FakeNow};
  CompositeCore auto_core{inner, strict, plans, deny, AutoRules(), &FakeNow, {}, &leased};
};

TEST_F(LeasedQuotaTest, ALargePlanTakesItsQuotaFromALease) {
  for (int i = 0; i < 5; ++i) {
    const auto r = RunOn(auto_core);
    EXPECT_TRUE(r.allowed);
    EXPECT_EQ(r.reason, Reason::kAllowed);
    EXPECT_EQ(r.limit, 1'000'000u);
    EXPECT_GT(r.reset_after_ms, 0u);  // next 00:00 UTC, computed locally on a hit
    EXPECT_LE(r.reset_after_ms, 86'400'000u);
  }
  const auto q = QuotaCalls();
  ASSERT_EQ(q.size(), 1u);  // one refill for five checks
  EXPECT_EQ(q[0].amount, 1000u);  // 0.1% of the daily limit
  EXPECT_EQ(q[0].limit, 1'000'000u);
  EXPECT_EQ(q[0].overage, Overage::kHard);  // a lease never takes overage
  EXPECT_EQ(redis.calls.size(), 6u);        // burst is still checked per call
}

// Leases refer to their rule, so the table of leased-quota rules is fixed and never freed: 64 plan
// sizes. A 65th size takes the exact path, which still answers, with the plan's own policy.
TEST_F(LeasedQuotaTest, A65thDistinctPlanSizeTakesTheExactPath) {
  for (int i = 0; i < 64; ++i) {
    redis.plan.fields["daily"] = std::to_string(1'000'000 + 1000 * i);
    EXPECT_TRUE(RunOn(auto_core).allowed);
  }
  EXPECT_EQ(QuotaCalls().size(), 64u);  // one lease refill per size
  redis.plan.fields["daily"] = "2000000";
  for (int i = 0; i < 3; ++i) EXPECT_TRUE(RunOn(auto_core).allowed);
  const auto q = QuotaCalls();
  ASSERT_EQ(q.size(), 64u + 3u);  // exact: one quota script per check
  EXPECT_EQ(q.back().amount, 1u);
  EXPECT_EQ(q.back().overage, Overage::kCap);
}

TEST_F(LeasedQuotaTest, PlansBelowTheThresholdAndTheStrictEngineStayExact) {
  redis.plan.fields["daily"] = std::to_string(kLeasedQuotaFrom - 1);
  for (int i = 0; i < 3; ++i) RunOn(auto_core);
  EXPECT_EQ(QuotaCalls().size(), 3u);
  redis.calls.clear();
  redis.plan.fields["daily"] = "1000000";
  CompositeCore no_leases{inner, strict, plans, deny, AutoRules(), &FakeNow};  // the strict engine
  for (int i = 0; i < 3; ++i) RunOn(no_leases);
  EXPECT_EQ(QuotaCalls().size(), 3u);
}

// Phase 2: a monthly, boosted or topped-up quota takes the exact path whatever its size, since only
// the script sees the Redis clock that ends a month, a boost or a top-up's period.
TEST_F(LeasedQuotaTest, MonthlyBoostedOrToppedUpLargePlansStayExact) {
  const std::vector<std::pair<std::string, std::string>> extras[] = {
      {{"period", "month"}}, {{"boost", "5000"}, {"boost_until", "9999999999999"}}, {{"topup", "5000"}, {"topup_period", "d20751"}}};
  for (const auto& more : extras) {
    redis.calls.clear();
    for (const auto& [field, value] : more) redis.plan.fields[field] = value;
    plans.Invalidate("cust:ent");
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(RunOn(auto_core).allowed);
    const auto q = QuotaCalls();
    ASSERT_EQ(q.size(), 3u) << more.front().first;  // one script per check, no lease refill
    EXPECT_EQ(q.back().amount, 1u) << more.front().first;
    for (const auto& [field, value] : more) redis.plan.fields.erase(field);
  }
}

// The plan is used up as far as leases go: the exact path decides, with the plan's policy and the
// request id, and a denial is cached as on the strict path.
TEST_F(LeasedQuotaTest, WhenTheLeaseIsOutTheExactPathDecides) {
  redis.replies["daily_quota"] = {.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000};
  const auto r = RunOn(auto_core, "r-1");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kQuotaExhausted);
  const auto q = QuotaCalls();
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[0].overage, Overage::kHard);  // the refill
  EXPECT_EQ(q[0].request_id, "");
  EXPECT_EQ(q[1].overage, Overage::kCap);  // the exact check
  EXPECT_EQ(q[1].amount, 1u);
  EXPECT_EQ(q[1].request_id, "r-1");
  EXPECT_EQ(deny.Size(), 1u);
  EXPECT_EQ(redis.returns.size(), 1u);  // the burst token went back
}

TEST_F(LeasedQuotaTest, OverageBeyondTheLeaseIsBilledOnTheExactPath) {
  redis.next["daily_quota"] = {{.allowed = false, .retry_after_ms = 60'000, .reset_after_ms = 60'000},  // lease
                               {.allowed = true, .reset_after_ms = 60'000, .overage = true}};         // exact
  const auto r = RunOn(auto_core);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kOverage);
  redis.next["daily_quota"] = {{.allowed = true, .reset_after_ms = 60'000, .overage = true}};
  EXPECT_EQ(RunOn(auto_core).reason, Reason::kOverage);  // the lease denies locally until the reset
  EXPECT_EQ(QuotaCalls().size(), 3u);                     // one refill, then an exact check each
}

TEST_F(LeasedQuotaTest, ALeaseThatCannotRefillFollowsThePlan) {  // after the lease runs out
  redis.replies["daily_quota"] = {.redis_error = true};
  redis.plan.fields["failure"] = "open";
  const auto r = RunOn(auto_core);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnverified);
}

TEST_F(LeasedQuotaTest, AnUpgradeLeasesUnderTheNewLimit) {
  RunOn(auto_core);
  redis.plan.fields["daily"] = "2000000";
  const auto r = RunOn(auto_core);
  EXPECT_EQ(r.limit, 2'000'000u);
  const auto q = QuotaCalls();
  ASSERT_EQ(q.size(), 2u);
  EXPECT_EQ(q[1].limit, 2'000'000u);
  EXPECT_EQ(q[1].amount, 2000u);
}

// ---- a merchant's limits (Phase 2, P3): its check rate, its capacity and the reserve ----

class MerchantLimitsTest : public CompositeCoreTest {
 protected:
  void SetUp() override {
    CompositeCoreTest::SetUp();
    for (const char* rule : {"merchant_checks", "merchant_capacity", "merchant_reserve"}) {
      redis.replies[rule] = {.allowed = true, .remaining = 10};
    }
  }
  CheckResult RunAs(RateLimiterCore& c, std::string_view rule = "customer", std::uint32_t cost = 1) {
    CheckResult out;
    bool called = false;
    c.Check({"acme|cust:42", rule, cost, {}, &limits}, [&](const CheckResult& r) {
      out = r;
      called = true;
    });
    EXPECT_TRUE(called);
    return out;
  }
  std::vector<std::string> Called() const {
    std::vector<std::string> out;
    for (const auto& c : redis.calls) out.push_back(c.rule);
    return out;
  }
  long Count(const std::string& rule) const {
    const auto called = Called();
    return std::count(called.begin(), called.end(), rule);
  }
  void Paid() { redis.plan.fields["fee"] = "1900"; }

  Rule checks{.name = "merchant_checks", .limit = 100, .refill_per_sec = 100, .failure_policy = FailurePolicy::kFailOpen};
  Rule capacity{.name = "merchant_capacity", .limit = 80, .refill_per_sec = 80, .failure_policy = FailurePolicy::kFailOpen};
  Rule reserve{.name = "merchant_reserve", .limit = 20, .refill_per_sec = 20, .failure_policy = FailurePolicy::kFailOpen};
  MerchantLimits limits{"acme", &checks, &capacity, &reserve};
  LeasedCore leased{redis, plans, Rules(), 1024, &FakeNow};
  CompositeCore leased_core{inner, strict, plans, deny, Rules(), &FakeNow, {}, &leased};
};

TEST_F(MerchantLimitsTest, OverTheCheckRateAnyRuleIsRefusedFirst) {
  redis.replies["merchant_checks"] = {.allowed = false, .retry_after_ms = 250};
  for (const char* rule : {"api_default", "customer"}) {
    const auto r = RunAs(core, rule);
    EXPECT_EQ(r.reason, Reason::kRateLimited) << rule;
    EXPECT_EQ(r.retry_after_ms, 250u);
  }
  EXPECT_TRUE(inner->rules.empty());
  EXPECT_EQ(redis.plan_reads, 0);
  EXPECT_EQ(Called(), (std::vector<std::string>{"merchant_checks", "merchant_checks"}));
  EXPECT_EQ(redis.calls[0].key, "acme");  // one bucket for all of the merchant's calls
}

TEST_F(MerchantLimitsTest, WithinItsLimitsACallTakesCapacityBetweenBurstAndQuota) {
  const auto r = RunAs(core, "customer", 3);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kAllowed);
  EXPECT_EQ(Called(), (std::vector<std::string>{"merchant_checks", "burst", "merchant_capacity", "daily_quota"}));
  EXPECT_EQ(redis.calls[0].amount, 1u);  // a check is one check, whatever its cost
  EXPECT_EQ(redis.calls[2].amount, 3u);  // capacity is taken by cost
}

TEST_F(MerchantLimitsTest, OverCapacityAFreePlanIsRefusedAndUsesNoQuota) {
  redis.replies["merchant_capacity"] = {.allowed = false, .retry_after_ms = 40};
  const auto r = RunAs(core, "customer", 2);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_EQ(r.retry_after_ms, 40u);
  EXPECT_EQ(Called(), (std::vector<std::string>{"merchant_checks", "burst", "merchant_capacity"}));  // never the reserve
  ASSERT_EQ(redis.returns.size(), 1u);
  EXPECT_EQ((std::tuple{redis.returns[0].rule, redis.returns[0].amount}), (std::tuple{std::string("burst"), 2u}));
}

TEST_F(MerchantLimitsTest, APlanWithAFeeTakesTheReserveWhenTheRestIsOut) {
  Paid();
  redis.replies["merchant_capacity"] = {.allowed = false, .retry_after_ms = 40};
  const auto r = RunAs(core);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(Called(), (std::vector<std::string>{"merchant_checks", "burst", "merchant_capacity", "merchant_reserve",
                                                "daily_quota"}));
  redis.replies["merchant_reserve"] = {.allowed = false, .retry_after_ms = 70};
  const auto out = RunAs(core);
  EXPECT_EQ(out.reason, Reason::kRateLimited);
  EXPECT_EQ(out.retry_after_ms, 70u);  // when the reserve has room again
}

TEST_F(MerchantLimitsTest, AnUnreachableCapacityDoesNotRefuse) {
  redis.replies["merchant_capacity"] = {.redis_error = true};
  EXPECT_TRUE(RunAs(core).allowed);
  redis.replies["merchant_checks"] = {.redis_error = true};
  EXPECT_TRUE(RunAs(core).allowed);
}

TEST_F(MerchantLimitsTest, AWhollyReservedCapacityServesOnlyPlansWithAFee) {
  limits.capacity = nullptr;
  EXPECT_EQ(RunAs(core).reason, Reason::kRateLimited);
  EXPECT_EQ(Count("merchant_reserve"), 0);
  Paid();
  EXPECT_TRUE(RunAs(core).allowed);
}

TEST_F(MerchantLimitsTest, OnTheLeasedEngineTheCheckRateIsALocalHit) {
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(RunAs(leased_core, "api_default").allowed);
  // One refill of a chunk (10% of 100), then hits: no Redis call per check.
  EXPECT_EQ(Count("merchant_checks"), 1);
  EXPECT_EQ(redis.calls[0].amount, 10u);
  EXPECT_EQ(inner->rules.size(), 5u);
}

TEST_F(MerchantLimitsTest, OnTheLeasedEngineAFloodOverCapacityCostsRedisNothing) {
  redis.replies["merchant_capacity"] = {.allowed = false, .retry_after_ms = 300};
  EXPECT_EQ(RunAs(leased_core).reason, Reason::kRateLimited);  // Redis says the capacity is out
  for (int i = 0; i < 5; ++i) {
    const auto r = RunAs(leased_core);
    EXPECT_EQ(r.reason, Reason::kRateLimited);
    EXPECT_GT(r.retry_after_ms, 0u);
  }
  EXPECT_EQ(Count("burst"), 1);  // no burst script, no token returned, no refill until the retry time
  EXPECT_EQ(redis.returns.size(), 1u);
  EXPECT_EQ(Count("merchant_capacity"), 1);
  Paid();  // a plan with a fee still reaches the reserve
  plans.Invalidate("acme|cust:42");
  EXPECT_TRUE(RunAs(leased_core).allowed);
  EXPECT_EQ(Count("merchant_reserve"), 1);
  g_now += 301 * kMs;  // the retry time passed: Redis is asked again
  redis.replies["merchant_capacity"] = {.allowed = true, .remaining = 10};
  EXPECT_TRUE(RunAs(leased_core).allowed);
  EXPECT_EQ(Count("merchant_capacity"), 2);
}

TEST_F(MerchantLimitsTest, OnTheLeasedEngineAWhollyReservedCapacityRefusesFreeCallsBeforeTheBurst) {
  const Rule* reserved = limits.reserve;
  limits.capacity = limits.reserve = nullptr;  // no capacity set: nothing refused
  EXPECT_TRUE(RunAs(leased_core).allowed);
  limits.reserve = reserved;
  EXPECT_EQ(RunAs(leased_core).reason, Reason::kRateLimited);
  EXPECT_EQ(Count("burst"), 1);  // only the first call's
  Paid();
  plans.Invalidate("acme|cust:42");
  EXPECT_TRUE(RunAs(leased_core).allowed);
  EXPECT_EQ(Count("merchant_reserve"), 1);
}

TEST_F(MerchantLimitsTest, OnTheLeasedEngineRedisSayingNoIsAnsweredLocally) {
  redis.replies["merchant_checks"] = {.allowed = false, .retry_after_ms = 300};
  EXPECT_EQ(RunAs(leased_core, "api_default").reason, Reason::kRateLimited);
  EXPECT_EQ(RunAs(leased_core, "api_default").reason, Reason::kRateLimited);
  EXPECT_EQ(Count("merchant_checks"), 1);  // until the retry time
  EXPECT_TRUE(inner->rules.empty());
}

}  // namespace
}  // namespace ratelimiter
