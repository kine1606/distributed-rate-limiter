# Plans

> Week 2 (PlanStore), Week 4 (PlanCache and invalidation) · Flow steps A4, B1, B5, B6, B9 · Status: Week 2 done: `plan.h` (`Plan`, `ParsePlan` with Free defaults) and `plan_store.h` (`HGETALL` per check); cache not started

## What this module does

A plan says what a customer bought: daily quota, burst size, overage policy and failure policy.
Plans live in Redis, written by the admin tool (Part B of the flow), and are read by the limiter on
the request path (Part A). The limiter never stores customers in `config/rules.yaml`; that file
holds rule templates, and plans hold per-customer numbers.

## Data model

```text
{cust:42}:plan   HASH
  plan         "free" | "pay" | "enterprise" | "suspended"
  daily        200          # daily quota
  burst        2            # token bucket capacity
  refill       2            # tokens per second
  overage      "hard" | "grace" | "cap"
  extra        0            # grace amount or overage cap, depending on `overage`
  failure      "open" | "closed"
```

Default when the hash is missing: treat the customer as Free. That default can only give away less
than the cheapest plan, so a missing hash is never a revenue leak.

## Files to create

| File | Contains | Week |
| --- | --- | --- |
| `plan.h` | `Plan` struct and parsing from a Redis hash | 2 |
| `plan_store.{h,cc}` | Async read of `{cust:N}:plan` through `RedisBackend` | 2 |
| `plan_cache.{h,cc}` | Sharded cache with TTL and invalidation | 4 |
| `plan_listener.{h,cc}` | Subscribes to `plan-changes`, calls `PlanCache::Invalidate` | 4 |

## Skeleton

```cpp
// plan.h
enum class OveragePolicy : std::uint8_t { kHard, kGrace, kCap };

struct Plan {
  std::string name{"free"};
  std::uint32_t daily{200};
  std::uint32_t burst{2};
  std::uint32_t refill_per_sec{2};
  OveragePolicy overage{OveragePolicy::kHard};
  std::uint32_t extra{0};
  FailurePolicy failure{FailurePolicy::kFailClosed};
  bool suspended{false};
};

// plan_cache.h
class PlanCache {
 public:
  PlanCache(PlanStore& store, std::chrono::milliseconds ttl);
  // Calls `done` inline on a cache hit; on a miss loads from Redis (single-flight per customer).
  void Get(std::string_view customer, std::function<void(const Plan&)> done);
  void Invalidate(std::string_view customer);   // from the Pub/Sub listener
 private:
  // HINT: N shards, each { mutex, unordered_map<string, Entry{Plan, expires_at}> }.
  //       A short per-shard mutex is fine here: plans change rarely and the hot path
  //       only takes it for a hash lookup. Measure before making it lock-free.
};
```

## Build it in this order

1. **Week 2, `PlanStore` without a cache.** Read the hash with `HGETALL` on every check. It costs
   one extra Redis call, and that is fine for the strict baseline. Correctness first.
2. **Week 2, parse defensively.** A missing field takes the Free default; an unknown `overage`
   value is a logged error and falls back to `hard`. Unit-test the parser with a broken hash.
3. **Week 4, add `PlanCache` with a TTL** of about 5 seconds. Now most checks need no plan read.
   Measure how much the Week 2 baseline improves from this alone; it is a nice small result.
4. **Week 4, add invalidation.** The admin tool publishes the customer ID on `plan-changes` after
   writing the hash. Every instance's listener drops that entry and its deny-cache entry. The TTL
   stays as a safety net when a message is missed.
5. **Week 4, test the upgrade path end to end:** exhaust a free customer, upgrade with `rlctl`,
   and check that the next request is allowed on every instance within one second.

## Hints

- **Why Pub/Sub plus TTL, not only one?** Pub/Sub is fast but loses messages while an instance is
  disconnected. A TTL alone means an upgrade can take up to the TTL to apply. Together you get fast
  in the normal case and bounded in the bad case, and you can state the bound in the thesis.
- **Single-flight here too.** A cold cache under load would otherwise send thousands of identical
  `HGETALL` calls for one popular customer. The same pattern as the lease refill applies.
- **Suspended is a plan, not a flag in the gateway only.** The gateway checks it too, but the
  limiter must deny with `SUSPENDED` in case a gateway's cache is stale.

## Done when

- [x] Week 2: strict engine reads the plan; Free default tested (`plan_test.cc`, `strict_core_test.cc`).
- [ ] Week 4: upgrade applies on all instances within one second (integration test).
- [ ] Week 4: cache hit ratio exported as a metric.
