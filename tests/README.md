# Tests

> Every week · Status: 60 tests passing (24 Lua script tests against Redis on 127.0.0.1:6380)

## How tests are organised

There are four levels. Lower levels are fast and run on every build; higher levels need Docker and
run before a push or at a weekly gate.

| Level | What it proves | Needs | Run with |
| --- | --- | --- | --- |
| Unit | One class is correct (parser, key builder, chunk policy, breaker) | nothing | `ctest -L unit` |
| Script | Lua scripts are correct against real Redis | one plain Redis on 6380 | `ctest -L redis` |
| Integration | The server, Redis Cluster and engines work together | Docker cluster | `ctest -L integration` |
| Evaluation | The thesis claims hold (accuracy, flood, faults) | Docker, several instances | scripts in `tests/eval/` |

Hint: add `LABELS` to each test in `tests/CMakeLists.txt` with `set_tests_properties`, so you can
run one level at a time. Script and integration tests skip, not fail, when Redis is unreachable
(`GTEST_SKIP()`), the way `lua_scripts_test.cc` already does.

## Files to create

| File | Level | Week | Covers |
| --- | --- | --- | --- |
| `lua_scripts_test.cc` | script | 1 | done: 13 cases |
| `keys_test.cc` | unit | 2 | Hash-tag key builders |
| `plan_test.cc` | unit | 2 | Plan parsing, Free default, broken hash |
| `strict_core_test.cc` | unit | 2 | `StrictCore` on `FakeBackend` |
| `redis_backend_it.cc` | integration | 2 | Backend against the cluster, `NOSCRIPT` fallback |
| `grpc_quota_it.cc` | integration | 2 | Week 2 gate: exactly 200 through gRPC |
| `lease_test.cc` | unit + TSan | 3 | `TryTake`, single-flight, chunk policy |
| `composite_core_test.cc` | unit | 4 | Every branch of the A2 decision tree |
| `overage_test.cc` | script | 4 | Request 2,001 on three policies |
| `breaker_test.cc` | unit | 5 | Breaker states with a fake clock |
| `tests/eval/*.sh` | evaluation | 6 | The nine tests in `docs/EVALUATION.md` |

## Sanitizers

```bash
scripts/test.sh tsan     # data races: run on every lease and batcher change from Week 3
scripts/test.sh asan     # use-after-free, leaks: catches dangling string_views
```

Hint: TSan slows code 5 to 15 times, so give timing-based tests generous timeouts under TSan, or
skip pure timing assertions there. Never "fix" a TSan report by adding a sleep.

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
