#include "core/composite_core.h"

#include <string>
#include <utility>

namespace ratelimiter {

CompositeCore::CompositeCore(std::shared_ptr<RateLimiterCore> inner, StrictCore& strict, PlanStore& plans,
                             DenyCache& deny, RuleTable rules, Clock clock, UsageObserver on_decision)
    : inner_(std::move(inner)),
      strict_(strict),
      plans_(plans),
      deny_(deny),
      rules_(std::move(rules)),
      now_(clock),
      on_decision_(std::move(on_decision)) {}

void CompositeCore::Check(const CheckRequest& request, CheckCallback done) {
  const auto it = rules_.find(request.rule);
  if (it == rules_.end() || it->second.quota_rule.empty()) {
    inner_->Check(request, std::move(done));
    return;
  }
  // rules_ is immutable, so these references stay valid for every callback below.
  const Rule& burst = rules_.find(it->second.burst_rule)->second;
  const Rule& quota = rules_.find(it->second.quota_rule)->second;
  if (on_decision_) {  // every answer below goes through `done`
    done = [this, key = std::string(request.key), cost = request.cost, done = std::move(done)](const CheckResult& r) {
      if (!r.replayed) on_decision_(key, r.reason, cost);
      done(r);
    };
  }

  // The request's views die when Check returns, so the callbacks keep copies.
  plans_.Get(request.key, [this, &burst, &quota, key = std::string(request.key), cost = request.cost,
                           id = std::string(request.request_id), done = std::move(done)](const Plan* p) mutable {
    if (p == nullptr) {
      done(RedisDownResult(quota));
      return;
    }
    const Plan plan = *p;  // `p` dies when this callback returns
    if (plan.suspended) {
      CheckResult r;
      r.reason = Reason::kSuspended;
      done(r);
      return;
    }
    if (const std::uint32_t reset = deny_.DeniedFor(key, QuotaStamp(plan), now_()); reset > 0) {
      CheckResult r;
      r.reason = Reason::kQuotaExhausted;
      r.limit = plan.daily;
      r.retry_after_ms = r.reset_after_ms = reset;
      done(r);
      return;
    }
    strict_.Check(burst, plan, key, cost, {},
                  [this, &burst, &quota, plan, key, cost, id = std::move(id), done = std::move(done)](
                      const CheckResult& b) mutable {
      if (!b.allowed) {
        done(b);
        return;
      }
      const bool burst_counted = b.reason == Reason::kAllowed;  // not when Redis was down and it failed open
      strict_.Check(quota, plan, key, cost, id,
                    [this, &burst, plan, key, cost, burst_counted, done = std::move(done)](const CheckResult& q) {
        if (!q.allowed && burst_counted) strict_.GiveBack(burst, plan, key, cost);
        // A replayed denial was decided under the plan of its first attempt, maybe not this one.
        if (q.reason == Reason::kQuotaExhausted && !q.replayed) {
          deny_.Deny(key, QuotaStamp(plan), now_(), q.reset_after_ms);
        }
        done(q);
      });
    });
  });
}

}  // namespace ratelimiter
