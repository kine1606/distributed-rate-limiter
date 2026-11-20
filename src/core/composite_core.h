#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

#include "core/leased_core.h"
#include "core/strict_core.h"
#include "lease/deny_cache.h"
#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"

namespace ratelimiter {

// Called once per customer decision (usage events, src/usage/README.md); not for the stored answer
// of a retried request ID, which was recorded the first time. Runs on the thread that answers.
using UsageObserver = std::function<void(std::string_view customer, Reason reason, std::uint32_t cost)>;

// One API call of a customer (src/core/README.md, Week 4). A customer rule (burst_rule and
// quota_rule set) is decided in this order, one plan read for all of it:
//   1. suspended plan: deny, no script call
//   2. deny cache hit: deny until the cached reset, no script call
//   3. burst: a denial answers at once and uses no quota
//   4. the merchant's capacity (P3), if the call names a merchant that set one: its shared part,
//      then for a plan with a fee its reserve; a denial answers kRateLimited, gives the burst token
//      back and uses no quota
//   5. daily quota under the plan's overage policy, deduplicated by request id; on a denial the
//      burst token goes back and the denial is cached
// When Redis cannot answer step 3 or 5 (or a quota lease refill), the plan's failure policy
// decides: fail open admits the call as kUnverified, fail closed denies it as kUnavailable. A
// merchant's capacity Redis cannot answer does not refuse. Every other rule goes to `inner` unchanged.
//
// Before all of that, for any rule, a call of a merchant with a check rate takes one token of it
// (P3); over the rate it is answered kRateLimited at once. Merchant buckets are leased when the
// engine leases (`quota_leases`), so N instances admit at most their limit + N x C, as any lease.
//
// Leased quota (Week 4): with `quota_leases` set and quota_mode: auto, a plan of kLeasedQuotaFrom
// a day or more takes step 5 from a lease of its daily counter, chunks of 0.1% of the limit, so
// N instances admit at most limit + N x C in a day. A lease takes only units within the plan;
// once it is out, the check takes the exact path, which applies the overage policy and request id.
// shortcut: a leased check does not deduplicate request ids, so a retry within the plan counts
// again; overage, which is billed, always goes the exact path.
//
// shortcut: burst goes through StrictCore (one Redis call), as LeasedCore already routes plan rules;
// move it to a per-customer lease once the plan cache gives each customer's chunk size.
class CompositeCore final : public RateLimiterCore {
 public:
  // `strict`, `plans` and `deny` must outlive this core. `rules` is the whole table: a customer
  // rule's burst and quota rules are looked up in it (LoadRules checked that they exist).
  // `quota_leases`, if set, must outlive this core too (main.cc passes the LeasedCore it wraps).
  CompositeCore(std::shared_ptr<RateLimiterCore> inner, StrictCore& strict, PlanStore& plans, DenyCache& deny,
                RuleTable rules, Clock clock = &SteadyNowNs, UsageObserver on_decision = {},
                LeasedCore* quota_leases = nullptr);

  void Check(const CheckRequest& request, CheckCallback done) override;

 private:
  void Decide(const CheckRequest& request, CheckCallback done);  // everything after the check rate
  void Quota(const Rule& burst, const Rule& quota, const Plan& plan, std::string key, std::uint32_t cost,
             std::string id, CheckCallback done);
  // Answers allowed, or the denial of the capacity a plan may use.
  void Capacity(const MerchantLimits& m, bool paid, std::uint32_t cost, CheckCallback next);
  // The same denial from leases alone, when Redis said recently that the capacity a plan may use is
  // out; nullopt: Capacity must ask.
  std::optional<CheckResult> CapacityOut(const MerchantLimits& m, bool paid, std::uint32_t cost);
  static const CheckResult kWhollyReserved;
  // `cost` tokens of one of a merchant's buckets: from a lease when the engine leases, else exactly.
  void TakeMerchant(const Rule& rule, std::string_view merchant, std::uint32_t cost, CheckCallback done);

  // The quota rule with one plan's daily limit and a hard policy: what a lease of that plan's
  // counter refills with. One per (quota rule, daily limit), made on first use and never freed,
  // because leases refer to it; nullptr once all slots hold other limits (then the check is exact).
  // Lock-free like LeaseTable: a slot goes from empty to a rule once, by CAS.
  const Rule* LeasedQuotaRule(const Rule& quota, std::uint32_t daily);

  struct LeasedRule {
    const Rule* quota;  // the template, for the lookup
    Rule rule;
  };
  struct LeasedRules {  // first member: destroyed after `inner_`, whose leases refer to these rules
    std::array<std::atomic<LeasedRule*>, 64> slots{};
    ~LeasedRules() {
      for (auto& s : slots) delete s.load(std::memory_order_relaxed);
    }
  } leased_rules_;
  const std::shared_ptr<RateLimiterCore> inner_;
  StrictCore& strict_;
  PlanStore& plans_;
  DenyCache& deny_;
  const RuleTable rules_;
  const Clock now_;
  const UsageObserver on_decision_;
  LeasedCore* const quota_leases_;
};

}  // namespace ratelimiter
