# Lease fast path and deny cache

> Week 3 (leases), Week 4 (deny cache, leased quota) · Flow steps A5, A7, A8 · Status: Week 4: the same leases serve large plans' daily quotas (chunks of 0.1%, `CompositeCore`; four instances admitted exactly 100,000 of 100,000, `tests/leased_quota_it.cc`); a deny lookup in a shard with no denial takes no lock. Week 5: `LeasedCore::ReturnLeases` gives unused token-bucket tokens back at shutdown. `lease_table.{h,cc}` done (lock-free `TryTake` and table, TSan clean); single-flight, early refill, top-up refills and local deny in `src/core/leased_core.cc`; adaptive chunk in `chunk_policy.h`; bound checked by the seeded harness `tests/lease_harness_test.cc`. interned 64-bit lease id (integer compare on lookup) and microbenchmarks in `bench/bench_lease.cc`. deny cache in `deny_cache.h` (Week 4: sharded, entries end at the Redis reset and are dropped when the plan changes). Not yet: waiter bound, eviction

## What this module does

A lease is a batch of burst tokens that one limiter instance has already taken from Redis and may
hand out locally. While the lease has tokens, a check is a single atomic subtraction in memory,
which takes about a microsecond instead of a Redis round trip. When the lease runs low, one refill
asks Redis for the next chunk. This is the part that makes high throughput possible, and also the
part where races hide, so TSan runs on it from day one.

The cost is accuracy: tokens sitting in one instance's lease cannot be used by another instance.
The worst case over-admission is about `instances × chunk` per refill interval. You derive and
measure that bound in the thesis.

## Files to create

| File | Contains |
| --- | --- |
| `lease.h` | `Lease`: one key's local tokens, aligned to a cache line |
| `lease_table.{h,cc}` | Sharded map from interned key to `Lease` |
| `refill_coordinator.{h,cc}` | Single-flight refills, waiter queues, chunk sizing |
| `chunk_policy.h` | Pure function: next chunk size from recent demand and the cap |
| `deny_cache.{h,cc}` | Week 4: customer to deny-until time |

## Skeleton

```cpp
// lease.h
struct alignas(64) Lease {                       // 64 = cache line: no false sharing
  std::atomic<std::int64_t> tokens{0};           // may go negative briefly; see hint
  std::atomic<bool> refill_in_flight{false};     // single-flight flag
  std::atomic<std::uint32_t> last_chunk{0};
  std::atomic<std::uint64_t> taken_this_window{0};  // demand estimate for chunk sizing
  std::uint64_t key_id{0};                       // interned key
};

// lease_table.h
class LeaseTable {
 public:
  explicit LeaseTable(std::size_t shards = 64);
  // Finds or creates the lease. Creation takes the shard lock; lookups of existing
  // leases must not (HINT: read-mostly map, or a lock only around insert).
  Lease& Get(std::string_view key);
  bool TryTake(Lease& l, std::uint32_t cost);     // CAS loop, no lock, no allocation
};

// refill_coordinator.h
class RefillCoordinator {
 public:
  RefillCoordinator(RedisBackend& redis, ChunkPolicy policy, std::size_t max_waiters);
  // Parks `done` until the refill returns; starts the refill only if none is in flight.
  void ParkAndRefill(Lease& l, const Rule& r, std::uint32_t cost, CheckCallback done);
  // Called on a hit when tokens fall below the low watermark: refill early, park nobody.
  void MaybeRefillEarly(Lease& l, const Rule& r);
};

// chunk_policy.h
struct ChunkPolicy {
  std::uint32_t min_chunk = 8;
  double max_fraction_of_limit = 0.10;   // cap: one instance never holds > 10% of capacity
  std::uint32_t Next(std::uint64_t recent_demand, std::uint32_t capacity) const;
};
```

## Build it in this order

1. **`TryTake` alone, with a unit test and a TSan test.** 8 threads take from one lease with 1,000
   tokens; exactly 1,000 must succeed. Use a compare-and-swap loop: read tokens, if less than cost
   return false, else try to swap in `tokens - cost`.
2. **`LeaseTable` with shards.** Shard by key hash. Leases must not move in memory once created,
   because threads hold references to them (use `std::unique_ptr<Lease>` in the map, or a deque).
3. **Single-flight refill.** The first thread that finds the lease empty flips `refill_in_flight`
   from false to true with `compare_exchange_strong` and sends the refill; others only park. When
   the reply comes, add the granted tokens, clear the flag, then serve parked waiters in order.
4. **Early refill.** On a hit, if tokens fall below 30% of `last_chunk` and no refill is in flight,
   start one. Most refills then finish before the lease is empty, so almost nobody waits.
5. **Chunk sizing (done, `chunk_policy.h`).** Starts at the cap, not `min_chunk`: only a dry lease
   doubles a chunk, so a moderately busy key started low would stay near the minimum (one Redis call
   per 8 checks instead of per 25). Doubles when checks found the lease dry while its refill was out; halves when the last
   chunk lasted more than 10 s; stays within [min(8, cap), cap]. Refills top the lease up to the
   chunk instead of adding a chunk, and checks costlier than the cap go to Redis directly: both keep
   an instance at no more than one chunk, which the bound needs (DESIGN.md section 6).
6. **Waiter bound and deadline.** If the waiter queue is full, answer `RESOURCE_EXHAUSTED`. If a
   refill takes longer than a few milliseconds, answer waiters with the failure policy (Week 5).
7. **Microbenchmark** `TryTake` with 1, 4 and 16 threads on one hot key and on 1,000 keys. The
   difference shows the cost of contention, which is a figure for Chapter 5.

## Deny cache (Week 4)

```cpp
class DenyCache {
 public:
  // true if the customer is known to be out of quota until `until_ms` (Redis time)
  bool IsDenied(std::string_view customer, std::uint32_t& reset_after_ms) const;
  void Deny(std::string_view customer, std::uint64_t until_ms);
  void Clear(std::string_view customer);    // on upgrade (plan-changes message)
};
```

Hint: store the reset time you got from Redis (`now + reset_after_ms`), not your local clock's
midnight. Then the deny cache expires at the same moment as the Redis counter. The flood test in
the evaluation measures how many Redis calls this saves; expect almost all of them.

## Hints

- **Why `int64_t` tokens?** A CAS loop never lets tokens go negative, but if you later switch to
  `fetch_sub` for speed, a signed type lets you detect and undo an overdraw instead of wrapping to
  four billion.
- **Memory order.** `TryTake` can use `memory_order_relaxed` for the counter itself; the refill
  that adds tokens and clears the flag should use release, and the reader of the flag acquire.
  Write one sentence in the code for each choice; reviewers and your advisor will ask.
- **Return leftovers on shutdown** (Week 5) through `return_lease.lua`.
- **Strict rules never touch this module.** If a rule is `strict: true`, the engine skips leases.

## The accuracy bound you will write up

With `N` instances and chunk size `C`, a key can have up to `N × C` tokens handed out locally that
Redis already counted as taken. When the bucket is close to empty, each instance can still admit
its leftover lease, so the worst case is `limit + N × C` admitted per refill interval. The
integration test in Week 6 hammers one key from all instances and asserts
`admitted <= limit + N × C`. The cap on `C` (a fraction of the limit) is how you tune it.

## Done when

- [x] TSan clean on all lease tests.
- [x] Single-flight proven: 500 concurrent misses on one key cause exactly one refill.
- [x] Seeded harness: 100,000 seeds keep admitted <= limit + N x C in every window (the default run
      is 2,000 seeds; `RL_HARNESS_SEEDS=N`, replay one with `RL_HARNESS_SEED=<seed>`).
- [x] Microbenchmark results saved (`results/2026-10-18_lease_microbench/`).
- [ ] Lease hit ratio exported as a metric (Week 5, Prometheus).
- [x] Week 4: deny cache tested, including clear on upgrade (`tests/deny_cache_test.cc`; an
      upgrade also lifts the denial without a message, because each entry keeps a stamp of the plan
      it was made under, `tests/composite_core_test.cc`).
