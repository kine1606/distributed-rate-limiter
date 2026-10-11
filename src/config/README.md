# Rule config and hot reload

> Week 2 (schema update), Week 5 (SIGHUP reload) · Flow step A4 · Status: YAML loader with tests

## What this module does

`config/rules.yaml` defines rule templates: which algorithm, whether strict, and defaults. The
per-customer numbers come from plans in Redis (see [src/plan](../plan/README.md)). Keeping the two
apart means a sign-up or an upgrade never needs a config change or a restart.

## Schema after Week 2

```yaml
rules:
  daily_quota:              # checked strictly against Redis on every request
    algorithm: quota
    strict: true
    limit_from: plan.daily  # numbers come from the customer's plan
  burst:
    algorithm: token_bucket
    strict: false           # served from the local lease
    limit_from: plan.burst
    refill_from: plan.refill
    lease:
      min_chunk: 8
      max_fraction: 0.10
  login_attempts:           # a fixed rule not tied to a plan, kept as a strict example
    algorithm: token_bucket
    strict: true
    limit: 5
    refill_per_sec: 0.083
    failure_policy: fail_closed
```

Hint: `Algorithm::kSlidingWindow` in `rule.h` can become `kQuota`, since the sliding window is cut
from scope. Update `rule_config.cc`, the tests, and the enum together in one commit.

## SIGHUP reload (Week 5)

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

- [ ] Week 2: new schema loaded and tested; invalid files produce a clear error.
- [ ] Week 5: reload under load with zero failed requests.
