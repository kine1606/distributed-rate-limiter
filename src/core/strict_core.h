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
// from the customer's plan, read first (one more round trip until the Week 4 plan cache).
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
      Ask(rule, rule, request.key, request.cost, std::move(done));
      return;
    }
    // request.key dies when Check returns, so the plan callback keeps its own copy.
    plans_.Get(request.key, [this, &rule, key = std::string(request.key), cost = request.cost,
                             done = std::move(done)](const Plan* plan) mutable {
      if (plan == nullptr) {
        done(RedisDownResult(rule));
        return;
      }
      Rule numbers = rule;
      if (rule.limit_from == PlanLimit::kDaily) {
        numbers.limit = plan->daily;
      } else {
        numbers.limit = plan->burst;
        numbers.refill_per_sec = plan->refill;
      }
      Ask(rule, numbers, key, cost, std::move(done));
    });
  }

 private:
  // `numbers` is `rule`, or a copy with the plan's limits filled in. Eval copies both and the key
  // before returning.
  void Ask(const Rule& rule, const Rule& numbers, std::string_view key, std::uint32_t cost, CheckCallback done) {
    redis_.Eval(numbers, key, cost, [&rule, limit = numbers.limit, done = std::move(done)](const ScriptResult& s) {
      if (s.redis_error) {
        done(RedisDownResult(rule));
        return;
      }
      CheckResult r;
      r.allowed = s.allowed;
      if (!s.allowed) r.reason = rule.algorithm == Algorithm::kQuota ? Reason::kQuotaExhausted : Reason::kRateLimited;
      r.limit = limit;
      r.remaining = s.remaining;
      r.retry_after_ms = s.retry_after_ms;
      r.reset_after_ms = s.reset_after_ms;
      done(r);
    });
  }

  RedisBackend& redis_;
  PlanStore& plans_;
  const RuleTable rules_;
};

}  // namespace ratelimiter
