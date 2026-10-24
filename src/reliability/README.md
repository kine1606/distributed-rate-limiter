# Reliability

> Week 5 · Flow step A12 · Status: Week 5 done: `circuit_breaker.h` (lock-free, fake-clock tests), `resilient_backend.{h,cc}` (breaker gating, retries of timeouts with jitter); failure policy per plan in `src/core/composite_core.cc`. Measurements: `docs/thesis/evidence/reliability_2026-10-21.md`. Week 6: a deadline per attempt (`RetryOptions::deadline`), found missing by the fault test (`docs/thesis/evidence/faults_2026-10-23.md`)

## What this module does

It decides what the limiter does when Redis is slow, gone, or overloaded, and makes sure that
decision is fast and predictable. The guiding rule from the plan: leases keep serving while they
last, then each plan's failure policy applies (paid fails open, free fails closed). Nothing here
may turn a Redis outage into a limiter outage or a retry storm.

## Files to create

| File | Contains |
| --- | --- |
| `circuit_breaker.{h,cc}` | Closed, open, half-open states around Redis calls |
| `retry.h` | Bounded retry with jittered backoff |
| `failure_policy.h` | Turns "no answer from Redis" into a `CheckResult` per plan |
| `resilient_backend.{h,cc}` | A `RedisBackend` decorator: timeout, retry, breaker |

## Skeleton

```cpp
class CircuitBreaker {
 public:
  struct Options {
    std::uint32_t failure_threshold = 20;          // consecutive failures to open
    std::chrono::milliseconds open_for{500};       // then allow one probe (half-open)
  };
  bool AllowRequest();          // false while open (except the single probe)
  void OnSuccess();
  void OnFailure();
  State state() const;          // exported as a metric
 private:
  std::atomic<State> state_{State::kClosed};
  std::atomic<std::uint32_t> failures_{0};
  std::atomic<std::int64_t> opened_at_ns_{0};
};

// Decorator: the engines keep using RedisBackend and get resilience for free.
class ResilientBackend final : public RedisBackend {
 public:
  ResilientBackend(RedisBackend& inner, CircuitBreaker& breaker, RetryOptions retry);
  void Eval(ScriptCall call, ReplyCallback done) override;
  // breaker open -> done({kUnavailable}) immediately, no network call
  // timeout      -> at most `retry.max_attempts`, delay = base * 2^n * random(0.5, 1.5)
};

inline CheckResult ApplyFailurePolicy(const Plan& p) {
  // fail open  -> allowed = true,  reason = kAllowed,     usage outcome = unverified
  // fail closed-> allowed = false, reason = kUnavailable, retry_after_ms = breaker open time
}
```

## Build it in this order

1. **Timeouts first.** Set the Redis client's socket timeout to 5 to 10 ms. Without a timeout,
   nothing else here can work, because a hung connection never reports failure.
2. **Circuit breaker** as a decorator around the backend, with unit tests driven by a fake clock:
   20 failures open it, one probe after 500 ms, success closes it.
3. **Failure policy** in the engine: when the reply is `kUnavailable` or `kTimeout` and the lease
   is empty, apply the plan's policy. Leases keep serving first.
4. **Retries** only for timeouts, at most one or two, with jitter. Never retry a script error; it
   will fail again.
5. **Fault test** in Docker: kill a master during load and record allowed, denied and unverified
   counts per plan, and the time until normal service. This is the Week 5 gate.

## Hints

- **Why jitter?** If 64 threads all retry after exactly 10 ms, Redis receives 64 calls at the same
  instant when it comes back. Random spread turns that spike into a ramp.
- **Why a single probe in half-open?** Letting all traffic through to test a recovering node can
  knock it over again. One probe is enough to learn whether it answers.
- **Strict rules feel outages first** because they have no lease. That is expected; say so in the
  failure-modes table of `DESIGN.md`.
- **Failover takes seconds.** Redis Cluster promotes a replica after `cluster-node-timeout`
  (default 15 s; set it to about 2 to 5 s in compose). Your breaker and leases must cover that gap.
- **`MOVED` and `ASK`** are handled by the cluster client; verify in the spike that it refreshes
  its slot map, and count redirects in a metric.

## What was built (Week 5)

- `failure_policy.h` was not needed: `CompositeCore` maps any "Redis down" answer to the plan's
  policy in one function (`PlanDown`), and `retry.h` lives inside `ResilientBackend`.
- Retries only where a repeat cannot loosen a limit, because a timed-out script may have run: plan
  reads, token-bucket and window scripts (a repeat takes one more token: stricter). Quota scripts
  are never retried: a repeat counts twice, or with a request id comes back `replayed` and the
  admitted call is never recorded for billing (found by the code review). Token returns neither.
- There is one breaker per group of 256 hash slots (`slot_breakers.h`): one breaker for the whole
  cluster opened for every customer when a master died (Week 5 gate). A dead master's calls fail by
  their deadline (50 ms) or at once when the connection is refused. An error reply (`MOVED`, a script error) is not a failure: Redis
  answered. While open, only the probe can close it (a late success from before is ignored). It
  reports opening and closing, not every failed probe.
- Code review of the Week 5 commits (the `code-review` skill, 10 findings): fixed quota retries,
  error replies feeding the breaker, late successes closing it, plans erased by invalidation during
  an outage, refills in flight at shutdown, batches larger than the in-flight bound, unchecked
  numeric settings. Not changed: the per-call copy for retries (already inside the measured 36,283
  checks/s; no profile shows it) and "Redis down" read from the reason code (no other cause exists).
- Timeout: 10 ms raised false timeouts under load with one Redis client (the I/O thread also
  finishes gRPC calls), and one timeout drops a whole connection. Four clients
  (`src/redis/sharded_backend.h`) and 50 ms gave none; see `docs/thesis/evidence/reliability_2026-10-21.md`.
- Deadline (Week 6): the client's socket timeout is an idle timeout per connection, re-armed by any
  reply, so a master answering every call 80 ms late under load was never timed out (p99 251 ms, no
  failure policy). `ResilientBackend` now answers an attempt as timed out at its deadline (the same
  50 ms) and drops the late reply. Deadlines wait in a lock-free queue in send order (Vyukov's
  intrusive MPSC queue: adding one is an atomic exchange), which the timer thread empties and checks
  in batches every 5 ms or more; each attempt is one allocation holding its own answer. A first
  version with a mutex cost 448 ns a call and serialised threads (`bench/bench_resilient`); lock-free
  it is 214 ns, 11 times the throughput at 12 threads, and within noise in the whole server. No false
  timeout at saturation or at 30,000/s open loop (`rl_redis_deadline_misses_total`).

## Done when

- [x] Breaker unit tests with a fake clock (`tests/circuit_breaker_test.cc`).
- [x] Kill-master test under load with replicas (Week 5 gate): `bench/fault_kill_master.sh`,
      `docs/thesis/evidence/week5_gate_2026-10-21.md`. It found the whole-cluster breaker opening for everyone: now one breaker per group of
      256 hash slots (`slot_breakers.h`).
- [ ] Added 20 ms latency (via `tc` or toxiproxy): p99 and breaker behaviour recorded.
