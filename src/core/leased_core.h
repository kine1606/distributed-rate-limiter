#pragma once

#include <cstddef>
#include <cstdint>

#include "core/strict_core.h"
#include "lease/lease_table.h"
#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// The main engine. Non-strict rules are served from a local lease: a check that finds tokens is one
// CAS (no Redis, no lock, no allocation). Redis is asked only to refill a lease, one chunk at a time
// and never twice at once for the same key. Strict rules, plan rules, unknown rules, and keys the
// lease table cannot hold go to StrictCore.
//
// Accuracy cost: tokens leased by this instance but not yet used are invisible to other instances,
// so N instances can admit up to limit + N x chunk (src/lease/README.md derives the bound).
class LeasedCore final : public RateLimiterCore {
 public:
  // `redis` and `plans` must outlive this core, and `redis` must be stopped before it is destroyed:
  // refill callbacks touch the leases.
  LeasedCore(RedisBackend& redis, PlanStore& plans, RuleTable rules,
             std::size_t lease_capacity = std::size_t{1} << 20);

  void Check(const CheckRequest& request, CheckCallback done) override;

 private:
  void Park(Lease& l, std::uint32_t cost, CheckCallback done);
  void StartRefill(Lease& l, std::uint32_t amount);
  void Refill(Lease& l, std::uint32_t amount, std::uint32_t partial_retry_ms);
  void OnRefill(Lease& l, std::uint32_t requested, std::uint32_t partial_retry_ms, const ScriptResult& s);

  RedisBackend& redis_;
  const RuleTable rules_;
  StrictCore strict_;
  LeaseTable leases_;
};

}  // namespace ratelimiter
