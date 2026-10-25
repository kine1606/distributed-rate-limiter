#pragma once

#include <string>
#include <utility>

#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// What a check answers when Redis could not be asked: the rule's failure policy.
inline CheckResult RedisDownResult(const Rule& rule) {
  CheckResult r;
  r.allowed = rule.failure_policy == FailurePolicy::kFailOpen;
  r.reason = r.allowed ? Reason::kUnverified : Reason::kUnavailable;
  r.limit = rule.limit;
  return r;
}

// One Redis script call per check: exact, and the baseline the lease engine is measured against.
// The backend picks the script from the rule's algorithm. A rule with limit_from takes its numbers
// from the customer's plan, read first (from the plan cache, or one more round trip without it).
class StrictCore final : public RateLimiterCore {
 public:
  // `redis` and `plans` must outlive this core.
  StrictCore(RedisBackend& redis, PlanStore& plans, RuleTable rules)
      : redis_(redis), plans_(plans), rules_(std::move(rules)) {}

  void Check(const CheckRequest& request, CheckCallback done) override {
    const auto it = rules_.find(request.rule);
    if (it == rules_.end()) {
      CheckResult r;
      r.reason = Reason::kUnknownRule;
      done(r);
      return;
    }
    const Rule& rule = it->second;  // rules_ is immutable, so the reference stays valid
    if (rule.limit_from == PlanLimit::kNone) {
      Ask(rule, rule, request.key, request.cost, request.request_id, std::move(done));
      return;
    }
    // request.key and request.request_id die when Check returns, so the plan callback keeps copies.
    plans_.Get(request.key, [this, &rule, key = std::string(request.key), cost = request.cost,
                             id = std::string(request.request_id), done = std::move(done)](const Plan* plan) mutable {
      if (plan == nullptr) {
        done(RedisDownResult(rule));
        return;
      }
      Check(rule, *plan, key, cost, id, std::move(done));
    });
  }

  // Checks `rule` (which must outlive the answer) with its numbers taken from `plan` when it has
  // limit_from. For CompositeCore, which reads the plan once for both of its rules.
  void Check(const Rule& rule, const Plan& plan, std::string_view key, std::uint32_t cost, std::string_view request_id,
             CheckCallback done) {
    Ask(rule, WithPlan(rule, plan), key, cost, request_id, std::move(done));
  }

  // Gives `tokens` back to the customer's bucket of the token-bucket `rule`: CompositeCore, after the
  // quota denied a request whose burst token was already taken. Best effort: if it fails, the bucket
  // is short by `tokens` until it refills.
  void GiveBack(const Rule& rule, const Plan& plan, std::string_view key, std::uint32_t tokens) {
    redis_.Return(WithPlan(rule, plan), key, tokens, [](const ScriptResult&) {});
  }

 private:
  // `rule`, with the plan's numbers if the rule takes them from the plan.
  static Rule WithPlan(const Rule& rule, const Plan& plan) {
    Rule numbers = rule;
    if (rule.limit_from == PlanLimit::kDaily) {
      numbers.limit = plan.daily;
      numbers.overage = plan.overage;
      numbers.overage_extra = plan.extra;
      numbers.quota_period = plan.period;
      numbers.boost = plan.boost;
      numbers.boost_until_ms = plan.boost_until_ms;
    } else if (rule.limit_from == PlanLimit::kBurst) {
      numbers.limit = plan.burst;
      numbers.refill_per_sec = plan.refill;
    }
    return numbers;
  }

  // `numbers` is `rule`, or a copy with the plan's limits filled in. Eval copies both, the key and
  // the request id before returning.
  void Ask(const Rule& rule, const Rule& numbers, std::string_view key, std::uint32_t cost,
           std::string_view request_id, CheckCallback done) {
    redis_.Eval(numbers, key, cost, request_id,
                [&rule, limit = numbers.limit, done = std::move(done)](const ScriptResult& s) {
      if (s.redis_error) {
        done(RedisDownResult(rule));
        return;
      }
      CheckResult r;
      r.allowed = s.allowed;
      if (!s.allowed) r.reason = rule.algorithm == Algorithm::kQuota ? Reason::kQuotaExhausted : Reason::kRateLimited;
      if (s.overage) r.reason = Reason::kOverage;
      r.limit = s.limit != 0 ? s.limit : limit;  // a quota reports its limit in force, a boost included
      r.remaining = s.remaining;
      r.retry_after_ms = s.retry_after_ms;
      r.reset_after_ms = s.reset_after_ms;
      r.replayed = s.replayed;
      done(r);
    });
  }

  RedisBackend& redis_;
  PlanStore& plans_;
  const RuleTable rules_;
};

}  // namespace ratelimiter
