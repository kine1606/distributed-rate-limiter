#include "core/composite_core.h"

#include <chrono>
#include <string>
#include <utility>

#include "lease/chunk_policy.h"

namespace ratelimiter {
namespace {

// Redis could not be asked: the answer carries the rule's failure policy (RedisDownResult).
bool RedisDown(const CheckResult& r) { return r.reason == Reason::kUnavailable || r.reason == Reason::kUnverified; }

// The customer's plan decides instead (Week 5): paid plans fail open, admitting the call as
// unverified (not counted, not billed); Free and unknown customers fail closed.
CheckResult PlanDown(const Plan& plan) {
  CheckResult r;
  r.allowed = plan.failure == FailurePolicy::kFailOpen;
  r.reason = r.allowed ? Reason::kUnverified : Reason::kUnavailable;
  r.limit = plan.daily;
  r.retry_after_ms = r.allowed ? 0 : 1000;  // about when the breaker probes Redis again
  return r;
}

}  // namespace

CompositeCore::CompositeCore(std::shared_ptr<RateLimiterCore> inner, StrictCore& strict, PlanStore& plans,
                             DenyCache& deny, RuleTable rules, Clock clock, UsageObserver on_decision,
                             LeasedCore* quota_leases)
    : inner_(std::move(inner)),
      strict_(strict),
      plans_(plans),
      deny_(deny),
      rules_(std::move(rules)),
      now_(clock),
      on_decision_(std::move(on_decision)),
      quota_leases_(quota_leases) {}

const Rule* CompositeCore::LeasedQuotaRule(const Rule& quota, std::uint32_t daily) {
  auto& slots = leased_rules_.slots;
  const std::uint64_t h = (daily ^ reinterpret_cast<std::uintptr_t>(&quota)) * 0x9e3779b97f4a7c15ULL;
  std::unique_ptr<LeasedRule> fresh;
  for (std::size_t i = 0; i < slots.size(); ++i) {
    std::atomic<LeasedRule*>& slot = slots[((h >> 58) + i) % slots.size()];
    LeasedRule* r = slot.load(std::memory_order_acquire);  // pairs with the release below: sees the rule
    if (r == nullptr) {
      if (!fresh) {
        fresh = std::make_unique<LeasedRule>(LeasedRule{&quota, quota});
        fresh->rule.limit = daily;
        fresh->rule.overage = Overage::kHard;  // a lease never takes overage
        fresh->rule.overage_extra = 0;
      }
      if (slot.compare_exchange_strong(r, fresh.get(), std::memory_order_release, std::memory_order_acquire)) {
        return &fresh.release()->rule;  // owned by leased_rules_ from now on
      }
    }
    if (r->quota == &quota && r->rule.limit == daily) return &r->rule;
  }
  return nullptr;
}

void CompositeCore::Check(const CheckRequest& request, CheckCallback done) {
  const MerchantLimits* m = request.merchant;
  if (m == nullptr || m->checks == nullptr) {
    Decide(request, std::move(done));
    return;
  }
  // The merchant's check rate (P3): one token per check, whatever its rule. The lease answers alone,
  // with no allocation, while it holds tokens or Redis said no recently (a flood over the rate);
  // otherwise it waits for a refill, and the request's views die when Check returns, so that path
  // keeps copies.
  if (quota_leases_ != nullptr) {
    if (const auto local = quota_leases_->TakeLocal(*m->checks, m->merchant, 1)) {
      if (!local->allowed) {
        done(*local);
        return;
      }
      Decide(request, std::move(done));
      return;
    }
  }
  TakeMerchant(*m->checks, m->merchant, 1,
               [this, key = std::string(request.key), rule = std::string(request.rule), cost = request.cost,
                id = std::string(request.request_id), m, done = std::move(done)](const CheckResult& r) mutable {
    if (!r.allowed && !RedisDown(r)) {  // over the check rate; Redis down: let it through
      done(r);
      return;
    }
    Decide(CheckRequest{key, rule, cost, id, m}, std::move(done));
  });
}

void CompositeCore::Decide(const CheckRequest& request, CheckCallback done) {
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
                           id = std::string(request.request_id), merchant = request.merchant,
                           done = std::move(done)](const Plan* p) mutable {
    if (p == nullptr) {  // never read and Redis down: unknown, so Free
      done(PlanDown(Plan{}));
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
    // A flood over the merchant's capacity stops here, before the burst script: past it, each
    // refused call would cost Redis that script and the token's return.
    if (merchant != nullptr) {
      if (const auto out = CapacityOut(*merchant, plan.paid, cost)) {
        done(*out);
        return;
      }
    }
    strict_.Check(burst, plan, key, cost, {},
                  [this, &burst, &quota, plan, key, cost, id = std::move(id), merchant, done = std::move(done)](
                      const CheckResult& b) mutable {
      if (RedisDown(b)) {  // no quota call: Redis is unlikely to answer it either
        done(PlanDown(plan));
        return;
      }
      if (!b.allowed) {
        done(b);
        return;
      }
      if (merchant == nullptr || (merchant->capacity == nullptr && merchant->reserve == nullptr)) {
        Quota(burst, quota, plan, std::move(key), cost, std::move(id), std::move(done));
        return;
      }
      // The merchant's capacity, after the customer's own burst and before its quota, so a call it
      // refuses is never counted or billed.
      // shortcut: a call the quota then refuses keeps its capacity token; the deny cache answers the
      // customer's next ones, so that is about one token per customer, instance and period. Return it
      // (a Redis call per denial) if a merchant's capacity is ever small next to its denials.
      Capacity(*merchant, plan.paid, cost,
               [this, &burst, &quota, plan, key, cost, id = std::move(id), done = std::move(done)](
                   const CheckResult& c) mutable {
        if (!c.allowed) {
          strict_.GiveBack(burst, plan, key, cost);  // the burst token was taken
          done(c);
          return;
        }
        Quota(burst, quota, plan, std::move(key), cost, std::move(id), std::move(done));
      });
    });
  });
}

void CompositeCore::Quota(const Rule& burst, const Rule& quota, const Plan& plan, std::string key, std::uint32_t cost,
                          std::string id, CheckCallback done) {
  CheckCallback finish = [this, &burst, plan, key, cost, done = std::move(done)](const CheckResult& answer) {
    const CheckResult q = RedisDown(answer) ? PlanDown(plan) : answer;
    if (!q.allowed) strict_.GiveBack(burst, plan, key, cost);  // the burst token was taken
    // A replayed denial was decided under the plan of its first attempt, maybe not this one.
    if (q.reason == Reason::kQuotaExhausted && !q.replayed) {
      deny_.Deny(key, QuotaStamp(plan), now_(), q.reset_after_ms);
    }
    done(q);
  };
  // Monthly, boosted and topped-up quotas take the exact path: only the script sees the Redis clock
  // that ends a month, a boost or a top-up's period, or an anchored billing month (P5). shortcut: a
  // large plan of that kind pays a script per check; lease them per period if such plans become common.
  const bool plain_daily = plan.period == QuotaPeriod::kDay && plan.boost == 0 && plan.topup == 0 && plan.anchor == 1;
  const Rule* leased = quota_leases_ != nullptr && quota.quota_auto && plan.daily >= kLeasedQuotaFrom && plain_daily
                           ? LeasedQuotaRule(quota, plan.daily)
                           : nullptr;
  if (leased == nullptr) {
    strict_.Check(quota, plan, key, cost, id, std::move(finish));
    return;
  }
  quota_leases_->CheckLease(*leased, {key, quota.name, cost, id},
                            [this, &quota, plan, key, cost, id, finish = std::move(finish)](CheckResult q) {
    if (q.reason == Reason::kRateLimited) {  // the lease is out: the exact path decides
      strict_.Check(quota, plan, key, cost, id, finish);
      return;
    }
    if (q.allowed && q.reset_after_ms == 0) {  // a lease hit knows no reset: next 00:00 UTC, as quota.lua
      using std::chrono::duration_cast, std::chrono::milliseconds, std::chrono::system_clock;
      constexpr std::int64_t kDayMs = 86'400'000;
      const std::int64_t now = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
      q.reset_after_ms = static_cast<std::uint32_t>(kDayMs - now % kDayMs);
    }
    finish(q);
  });
}

// A plan without a fee when the merchant reserved its whole capacity for plans with one.
const CheckResult CompositeCore::kWhollyReserved{.reason = Reason::kRateLimited, .retry_after_ms = 1000};

void CompositeCore::Capacity(const MerchantLimits& m, bool paid, std::uint32_t cost, CheckCallback next) {
  const auto reserve = [this, &m, paid, cost](CheckResult denied, CheckCallback next) {
    if (!paid || m.reserve == nullptr) {  // a Free plan never takes the reserve
      next(denied);
      return;
    }
    TakeMerchant(*m.reserve, m.merchant, cost, [next = std::move(next)](const CheckResult& r) {
      next(r.allowed || RedisDown(r) ? CheckResult{.allowed = true} : r);
    });
  };
  if (m.capacity == nullptr) {  // the whole capacity is reserved
    reserve(kWhollyReserved, std::move(next));
    return;
  }
  TakeMerchant(*m.capacity, m.merchant, cost, [reserve, next = std::move(next)](const CheckResult& r) mutable {
    if (r.allowed || RedisDown(r)) {  // Redis down: the capacity cannot be known, so it does not refuse
      next(CheckResult{.allowed = true});
      return;
    }
    reserve(r, std::move(next));
  });
}

std::optional<CheckResult> CompositeCore::CapacityOut(const MerchantLimits& m, bool paid, std::uint32_t cost) {
  if (quota_leases_ == nullptr || (m.capacity == nullptr && m.reserve == nullptr)) return std::nullopt;  // none set
  auto out = m.capacity != nullptr ? quota_leases_->DeniedLocal(*m.capacity, m.merchant, cost) : kWhollyReserved;
  if (out && paid && m.reserve != nullptr) out = quota_leases_->DeniedLocal(*m.reserve, m.merchant, cost);
  return out;
}

void CompositeCore::TakeMerchant(const Rule& rule, std::string_view merchant, std::uint32_t cost, CheckCallback done) {
  if (quota_leases_ != nullptr) {
    quota_leases_->CheckLease(rule, CheckRequest{merchant, rule.name, cost}, std::move(done));
  } else {
    strict_.Check(rule, Plan{}, merchant, cost, {}, std::move(done));
  }
}

}  // namespace ratelimiter
