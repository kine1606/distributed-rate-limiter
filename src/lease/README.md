# Lease fast path and deny cache

> Week 3 (leases), Week 4 (deny cache) · Flow steps A5, A7, A8 · Status: not started

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
5. **Chunk sizing.** Start at `min_chunk`. Double while the lease empties faster than one refill
   round trip; halve when tokens sit unused. Never exceed `max_fraction_of_limit × capacity`.
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

- [ ] TSan clean on all lease tests.
- [ ] Single-flight proven: 500 concurrent misses on one key cause exactly one refill.
- [ ] Microbenchmark results saved; lease hit ratio exported as a metric.
- [ ] Week 4: deny cache tested, including clear on upgrade.
