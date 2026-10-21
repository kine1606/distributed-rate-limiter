# Tests

> Every week · Status: 210 tests: 160 unit, 33 script (plain Redis on 127.0.0.1:6380), 17 integration (dev cluster on 127.0.0.1:7001); one executable per level; release, ASan and TSan clean (gRPC tests skip under TSan: Ubuntu's gRPC is not instrumented)

## How tests are organised

There are four levels. Lower levels are fast and run on every build; higher levels need Docker and
run before a push or at a weekly gate.

| Level | What it proves | Needs | Run with |
| --- | --- | --- | --- |
| Unit | One class is correct (parser, key builder, chunk policy, breaker) | nothing | `ctest -L unit` |
| Script | Lua scripts are correct against real Redis | one plain Redis on 6380 | `ctest -L redis` |
| Integration | The server, Redis Cluster and engines work together | dev cluster on 7001 (`RL_TEST_CLUSTER`) | `ctest -L integration` |
| Evaluation | The thesis claims hold (accuracy, flood, faults) | Docker, several instances | scripts in `tests/eval/` |

Each level is its own executable (`rl_unit_tests`, `rl_redis_tests`, `rl_integration_tests`) with
a ctest label. Script and integration tests skip, not fail, when Redis is unreachable
(`GTEST_SKIP()`).

## Files to create

| File | Level | Week | Covers |
| --- | --- | --- | --- |
| `lua_scripts_test.cc` | script | 1, 4 | done: 33 cases, including overage policies, request ID and `return_lease.lua` |
| `keys_test.cc` | unit | 2, 4 | done: hash-tag key builders, request key shares the tag |
| `plan_test.cc` | unit | 2 | done: plan parsing, Free default, broken hash |
| `strict_core_test.cc` | unit | 2, 4 | done: `StrictCore` on `FakeBackend`, plan rules, replayed answers, one plan read with the cache |
| `service_test.cc` | unit | 4 | done: gRPC service on an in-process channel: validation before the engine, status versus reason, late answers from other threads, batch order and limits |
| `redis_backend_it.cc` | integration | 2 | done: backend against the cluster, `NOSCRIPT` fallback |
| `grpc_quota_it.cc` | integration | 2, 4 | done: Week 2 gate (exactly 200), Week 4 gate (request 2,001 on three policies), suspended, retried request counted once |
| `lease_test.cc` | unit + TSan | 3 | done: `TryTake`, lease table, chunk policy |
| `batcher_test.cc` | unit + TSan | 3, 4 | done: timer and adaptive batching, every caller its own reply, drain on destruction, queued calls keep their rule numbers and request ID |
| `lease_harness_test.cc` | unit | 3 | done: seeded fake Redis and fake clock, bound in every window; `RL_HARNESS_SEEDS`, `RL_HARNESS_SEED` |
| `composite_core_test.cc` | unit | 4 | done: every branch of the A2 decision tree, usage events once per decision, replays neither recorded nor cached, leased quota (lease hit, threshold, exact path when out, overage, upgrade) |
| `deny_cache_test.cc` | unit + TSan | 4 | done: reset time, plan change, clear, sweep, replacement, concurrent use |
| `plan_store_test.cc` | unit + TSan | 4 | done: plan cache hits, TTL, single flight, invalidation racing a read, errors not cached, sweep, per-thread copies invalidated, concurrent use |
| `usage_log_test.cc` | unit + TSan | 4 | done: sums per customer and outcome, escaping, periodic flush, every event written or counted as dropped |
| `plan_change_it.cc` | integration | 4 | done: steps B5 and B9 through `rlctl` on two instances within one second; `rlctl` input checks |
| `leased_quota_it.cc` | integration | 4 | done: Week 4 gate, four instances on one leased customer stay within limit + N × C |
| `metrics_test.cc` | unit | 5 | done: counters across threads, histogram text, label escaping, the `/metrics` endpoint over a socket |
| `circuit_breaker_test.cc` | unit + TSan | 5 | done: breaker states on a fake clock, one probe, retries of timeouts only where safe, open breaker answers without Redis, stopping answers waiting retries |
| `overage_test.cc` | script | 4 | built inside `lua_scripts_test.cc` and `grpc_quota_it.cc` instead |
| `breaker_test.cc` | unit | 5 | built as `circuit_breaker_test.cc` |
| `tests/eval/*.sh` | evaluation | 6 | The nine tests in `docs/EVALUATION.md` |

## Sanitizers

```bash
scripts/test.sh tsan     # data races: run on every lease and batcher change from Week 3
scripts/test.sh asan     # use-after-free, leaks: catches dangling string_views
```

Hint: TSan slows code 5 to 15 times, so give timing-based tests generous timeouts under TSan, or
skip pure timing assertions there. Never "fix" a TSan report by adding a sleep.

Two TSan facts on this setup. The test binaries run under `setarch -R` (set in `tests/CMakeLists.txt`),
because GCC 13's TSan aborts with "unexpected memory mapping" under the WSL2 kernel's address
randomisation. And `grpc_quota_it.cc` skips itself under TSan: Ubuntu's gRPC is not instrumented,
so its arena allocator shows up as a race in every call (861 reports, none with both sides in our
code). The backend integration tests, which have no gRPC, run under TSan and are clean.

## Writing a good test here

- **Name the behaviour, not the function:** `QuotaUpgradeMidDayKeepsUsage` says what breaks if it
  fails; `TestQuota3` does not.
- **One reason to fail per test.** It makes a red test tell you what is wrong without a debugger.
- **Concurrency tests count exact outcomes.** "8 threads, 400 attempts, capacity 100, exactly 100
  admitted" is a real assertion. "No crash" is not.
- **Unique keys per test** (`{t1}`, `{t2}`) so tests do not interfere, and delete them at the start
  rather than the end, so a failed run does not poison the next one.

## Done when (Week 6 gate)

- [ ] All levels green in release, TSan and ASan builds.
- [ ] Every evaluation test in `docs/EVALUATION.md` has a script and a saved result.
