# Rule config and hot reload

> Week 2 (schema update), Week 5 (SIGHUP reload) · Flow step A4 · Status: Week 2 schema done: `algorithm: quota` and `limit_from: plan.daily | plan.burst`, validated with clear errors. Week 4: `quota_mode: strict | auto` on quota rules (auto: plans of 100,000 a day or more are leased by `CompositeCore`). Week 5: SIGHUP reload done, by swapping the whole engine (below)

## What this module does

`config/rules.yaml` defines rule templates: which algorithm, whether strict, and defaults. The
per-customer numbers come from plans in Redis (see [src/plan](../plan/README.md)). Keeping the two
apart means a sign-up or an upgrade never needs a config change or a restart.

## Schema after Week 2

As built (`config/rules.yaml` has the full file):

```yaml
rules:
  daily_quota:              # checked strictly against Redis on every request
    algorithm: quota
    limit_from: plan.daily  # the limit comes from the customer's plan hash
    strict: true            # required for quota rules
  burst:
    algorithm: token_bucket
    limit_from: plan.burst  # capacity plan.burst, refill plan.refill per second
    strict: true            # per-customer leases need the plan cache (Week 4)
  login_attempts:           # a fixed rule not tied to a plan
    algorithm: sliding_window
    limit: 5
    window_ms: 60000
    strict: true
    failure_policy: fail_closed
```

Algorithms: `token_bucket` (needs `refill_per_sec` unless `limit_from: plan.burst`), `fixed_window`
and `sliding_window` (need `window_ms`), `quota` (needs `strict: true`; `limit` or
`limit_from: plan.daily`). `limit_from` must fit the algorithm: `plan.daily` only with `quota`,
`plan.burst` only with `token_bucket`. A leased rule may set `lease_chunk_pct` (1 to 100, default 10): the
largest share of the limit one instance holds, so the bound limit + N × C is chosen per rule (Week 6).
`Algorithm` values index the script and key-suffix tables in
`src/redis/redispp_backend.cc` and `src/redis/keys.h`, so new ones are appended.

Not built from the original sketch: `refill_from` (`plan.burst` always brings `plan.refill`) and the
`lease:` block (the chunk policy is Week 3's adaptive sizing).

## SIGHUP reload (Week 5)

As built (`src/server/main.cc`, `RateLimiterService::SetCore`): the signal handler sets a flag; the
main loop, which reads it every 100 ms, parses the whole file with `LoadRules` (an error is logged
and the old rules stay live), builds a new engine from the new table, and switches the service to
it with one atomic pointer store. Checks from then on use the new engine; a check already in the old
one finishes there. One second later the old engine returns its unused leased tokens, as at
shutdown. Replaced engines are kept until exit, so no check meets a freed one. `CheckMetrics`
keeps every rule's series and adds those of new rules. Measured under load:
`docs/thesis/evidence/reload_2026-10-22.md` (no failed call during 9 reloads at 10,000 checks/s,
p99 0.72 to 1.33 ms for a pass with three reloads in 24 s; no measurable cost per check).

Not built from the sketch below: a `RuleStore` read on every check. The engines keep references into
their rule table (each lease refers to its rule), so swapping the table under them would leave
leases on old rules; a new engine per table keeps the table immutable for its whole life.

The original sketch:

```cpp
class RuleStore {
 public:
  std::shared_ptr<const RuleTable> Current() const {           // hot path: one atomic load
    return std::atomic_load_explicit(&table_, std::memory_order_acquire);
  }
  bool Reload(const std::string& path);   // parse fully, then swap; on error keep the old table
 private:
  std::shared_ptr<const RuleTable> table_;
};
```

Build it in this order:

1. Parse into a new table. If parsing throws, log the error and keep the old table. A typo in the
   file must never take the service down.
2. Swap with an atomic store. Requests that already hold the old `shared_ptr` finish with the old
   rules; new requests see the new ones. No request is dropped, which is what the spec asks.
3. The signal handler only sets a flag. A normal thread sees the flag and calls `Reload`, because
   almost nothing is safe to do inside a signal handler.
4. Test: run load, change a limit, send SIGHUP, and check that the change applies with zero errors.

Hint: the free functions `std::atomic_load`/`std::atomic_store` on `shared_ptr` are deprecated in
C++20 (you get a warning). The C++20 replacement is `std::atomic<std::shared_ptr<const RuleTable>>`,
which libstdc++ implements with a small internal lock. Either is fine here: the hot path does one
load per request, and reloads are rare. Measure it in the microbenchmark if you are unsure.

## Done when

- [x] Week 2: new schema loaded and tested; invalid files produce a clear error.
- [x] Week 5: reload under load with zero failed requests (`bench/reload_under_load.sh`).
