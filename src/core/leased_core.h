#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "core/strict_core.h"
#include "lease/lease_table.h"
#include "ratelimiter/clock.h"
#include "ratelimiter/core.h"
#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// Called after a lease's chunk size changed, from the refill path (never on a hit).
using ChunkObserver = std::function<void(const Lease& lease, std::uint32_t from, std::uint32_t to)>;

// Mechanisms the Week 7 ablation switches off one at a time (bench/ablation.sh); both on in production.
struct LeaseTuning {
  bool early_refill = true;    // refill at 30% of the chunk, before the lease runs dry
  bool adaptive_chunk = true;  // the chunk follows demand; off: always the cap
};

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
             std::size_t lease_capacity = std::size_t{1} << 20, Clock clock = &SteadyNowNs,
             ChunkObserver on_chunk_change = {}, LeaseTuning tuning = {});

  void Check(const CheckRequest& request, CheckCallback done) override;

  // Serves `request` from a lease of `rule`, whatever request.rule names: CompositeCore's leased
  // quota passes a quota rule holding one plan's limit. `rule` must outlive this core. What the
  // lease cannot serve (a check bigger than a chunk, a full lease table) goes to the exact path
  // under request.rule; a denial from the lease itself answers kRateLimited.
  void CheckLease(const Rule& rule, const CheckRequest& request, CheckCallback done);

  // Shutdown (Week 5): gives the unused tokens of every token-bucket lease back to Redis
  // (lua/return_lease.lua) and waits for the answers (1.5 x `wait` at most in all), so a restart does not burn part
  // of each bucket. Refills still in flight are waited for first (within the same `wait`), so the
  // tokens they bring are returned too. Quota leases keep their units: at most one chunk (0.1% of the day) each, and
  // giving units back to a day would need a script of its own. Call it once checks have stopped.
  struct Returned {
    std::uint64_t leases;  // leases that held tokens
    std::uint64_t tokens;  // tokens sent back
    std::uint64_t failed;  // returns that failed or did not answer in time (those tokens are lost)
    std::uint64_t refills_unfinished;  // refills still out when the wait ended: their tokens are lost
  };
  Returned ReturnLeases(std::chrono::milliseconds wait);

  // Slow-path counters; a hit touches neither. Lease hit ratio = 1 - parked / checks served.
  struct Stats {
    std::uint64_t parked;   // checks that waited for a refill
    std::uint64_t refills;  // refill calls sent to Redis
  };
  Stats GetStats() const {
    return {parked_.load(std::memory_order_relaxed), refills_.load(std::memory_order_relaxed)};
  }

 private:
  void Park(Lease& l, std::uint32_t cost, CheckCallback done);
  void StartRefill(Lease& l, std::uint32_t target);  // tops the lease up to `target` tokens
  void Refill(Lease& l, std::uint32_t amount, std::uint32_t partial_retry_ms);
  void OnRefill(Lease& l, std::uint32_t requested, std::uint32_t partial_retry_ms, const ScriptResult& s);

  RedisBackend& redis_;
  std::atomic<std::uint64_t> parked_{0}, refills_{0};
  const Clock now_;
  const ChunkObserver on_chunk_change_;
  const LeaseTuning tuning_;
  const RuleTable rules_;
  StrictCore strict_;
  LeaseTable leases_;
};

}  // namespace ratelimiter
