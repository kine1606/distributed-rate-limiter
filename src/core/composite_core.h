#pragma once

#include <memory>

#include "core/leased_core.h"
#include "core/strict_core.h"
#include "lease/deny_cache.h"
#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"

namespace ratelimiter {

// One API call of a customer (src/core/README.md, Week 4). A customer rule (burst_rule and
// quota_rule set) is decided in this order, one plan read for all of it:
//   1. suspended plan: deny, no script call
//   2. deny cache hit: deny until the cached reset, no script call
//   3. burst: a denial answers at once and uses no quota
//   4. daily quota under the plan's overage policy, deduplicated by request id; on a denial the
//      burst token goes back and the denial is cached
// Every other rule goes to `inner` unchanged.
//
// shortcut: burst goes through StrictCore (one Redis call), as LeasedCore already routes plan rules;
// move it to a per-customer lease once the plan cache gives each customer's chunk size.
class CompositeCore final : public RateLimiterCore {
 public:
  // `strict`, `plans` and `deny` must outlive this core. `rules` is the whole table: a customer
  // rule's burst and quota rules are looked up in it (LoadRules checked that they exist).
  CompositeCore(std::shared_ptr<RateLimiterCore> inner, StrictCore& strict, PlanStore& plans, DenyCache& deny,
                RuleTable rules, Clock clock = &SteadyNowNs);

  void Check(const CheckRequest& request, CheckCallback done) override;

 private:
  const std::shared_ptr<RateLimiterCore> inner_;
  StrictCore& strict_;
  PlanStore& plans_;
  DenyCache& deny_;
  const RuleTable rules_;
  const Clock now_;
};

}  // namespace ratelimiter
