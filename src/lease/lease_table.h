#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "lease/chunk_policy.h"
#include "ratelimiter/rule.h"
#include "ratelimiter/types.h"

namespace ratelimiter {

// A check parked until its lease's refill comes back from Redis.
struct Waiter {
  std::uint32_t cost;
  CheckCallback done;
};

// One (rule, key)'s local tokens: units this instance already took from Redis and may hand out
// without asking again. alignas(64): hot leases used from different cores never share a cache line.
struct alignas(64) Lease {
  Lease(const Rule& r, std::string_view k, std::uint64_t key_id = 0) : chunk(ChunkCap(r)), id(key_id), rule(r), key(k) {}

  // Touched by every check of this key: atomics only.
  std::atomic<std::int64_t> tokens{0};
  std::atomic<std::int64_t> deny_until_ns{0};  // engine clock; Redis said no until then
  std::atomic<bool> refill_in_flight{false};   // single flight: one refill per lease at a time
  // Tokens per refill. Starts at the cap: starting low would keep a moderately busy key near the
  // minimum chunk, because only a dry lease doubles it. Written only by the refill owner.
  std::atomic<std::uint32_t> chunk;

  // The interned key: a 64-bit hash of (rule, key) computed once, when the lease is created. A
  // lookup compares it as an integer and reads the key string only when it matches.
  const std::uint64_t id;

  const Rule& rule;  // lives in the engine's immutable RuleTable
  const std::string key;

  // Slow path only: checks that found the lease empty, and the refill callback.
  std::mutex mu;
  std::vector<Waiter> waiters;
  std::int64_t last_refill_ns{0};  // when the last full refill landed (0: never); under mu
};

// Takes `cost` tokens if the lease has them; returns the tokens left, or -1 (nothing taken).
// Lock-free CAS loop that never lets tokens go below zero. Relaxed ordering: the counter is the
// only data a hit reads, and nothing else is published through it.
inline std::int64_t TryTake(Lease& l, std::uint32_t cost) {
  std::int64_t t = l.tokens.load(std::memory_order_relaxed);
  while (t >= cost) {
    if (l.tokens.compare_exchange_weak(t, t - cost, std::memory_order_relaxed)) return t - cost;
  }
  return -1;
}

// Fixed-capacity open-addressing map from (rule, key) to Lease. Lookups and inserts are lock-free:
// a slot goes from empty to a Lease exactly once (CAS) and is never cleared or moved, so a Lease
// stays valid for the table's lifetime.
// shortcut: leases are never evicted, so memory grows with distinct keys up to `capacity`, and a
// key whose probe window is full gets nullptr (the caller then uses the strict path). Add idle
// eviction with safe memory reclamation before running with millions of distinct keys.
class LeaseTable {
 public:
  explicit LeaseTable(std::size_t capacity);  // rounded up to a power of two, at least kMaxProbe
  ~LeaseTable();

  LeaseTable(const LeaseTable&) = delete;
  LeaseTable& operator=(const LeaseTable&) = delete;

  // The lease of (rule, key), created empty on first sight; nullptr if the table has no room.
  // Allocates only when it creates.
  Lease* FindOrCreate(const Rule& rule, std::string_view key);

 private:
  static constexpr std::size_t kMaxProbe = 64;
  std::size_t mask_;
  std::unique_ptr<std::atomic<Lease*>[]> slots_;  // the table owns every non-null Lease
};

}  // namespace ratelimiter
