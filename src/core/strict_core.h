#pragma once

#include <string>
#include <utility>

#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// What a check answers when Redis could not be asked: the rule's failure policy.
inline CheckResult RedisDownResult(const Rule& rule) {
  CheckResult r;
  r.allowed = rule.failure_policy == FailurePolicy::kFailOpen;
  r.reason = r.allowed ? Reason::kAllowed : Reason::kUnavailable;
  r.limit = rule.limit;
  return r;
}

// One Redis script call per check: exact, and the baseline the lease engine is measured against.
// The backend picks the script from the rule's algorithm.
class StrictCore final : public RateLimiterCore {
 public:
  // `redis` must outlive this core.
  StrictCore(RedisBackend& redis, RuleTable rules) : redis_(redis), rules_(std::move(rules)) {}

  void Check(const CheckRequest& request, CheckCallback done) override {
    const auto it = rules_.find(request.rule);
    if (it == rules_.end()) {
      CheckResult r;
      r.reason = Reason::kUnknownRule;
      done(r);
      return;
    }
    const Rule& rule = it->second;  // rules_ is immutable, so the reference stays valid

    // Eval copies the key before returning, so request's string_views may die after this call.
    redis_.Eval(rule, request.key, request.cost, [&rule, done = std::move(done)](const ScriptResult& s) {
      if (s.redis_error) {
        done(RedisDownResult(rule));
        return;
      }
      CheckResult r;
      r.allowed = s.allowed;
      r.reason = s.allowed ? Reason::kAllowed : Reason::kRateLimited;
      r.limit = rule.limit;
      r.remaining = s.remaining;
      r.retry_after_ms = s.retry_after_ms;
      done(r);
    });
  }

 private:
  RedisBackend& redis_;
  const RuleTable rules_;
};

}  // namespace ratelimiter
