# Evidence: verification of Weeks 1 to 6, coverage, and measured speed-ups (2026-10-23)

After the `v1.0` freeze, a review of Weeks 1 to 6 looked for four things: work left open, code no
test exercises, locks left on busy paths, and speed-ups. The freeze allows fixes and measured
performance changes; everything below is one or the other.

## 1. Work left open

- **Every Week 1 to 6 item is done.** One gate is only partly met, Week 3's.
  - Its p99 target is now met: the open-loop generator measured 0.83 ms at 10,000 checks/s
    (`week5_gate_2026-10-21.md`); the first measurement used `ghz` and timed calls from their send.
  - Its "4× the strict baseline on one instance" target cannot be met on this laptop: gRPC with no
    limiter runs at 3.3× that baseline.
- **Six shortcuts are recorded in the code** (`grep -rn "shortcut:" src`), each with its limit:
  - plan rules, so a customer's burst, take one Redis call per check;
  - a leased quota does not deduplicate request ids;
  - leases are never evicted;
  - the deny cache locks a shard that holds a denial;
  - a replaced engine stays in memory until exit.

  The noisy-neighbour test (`noisy_neighbour_2026-10-23.md`) measured the cost of the first one.

## 2. Coverage

`scripts/coverage.sh` builds with `--coverage` (preset `coverage`), runs every test (unit, script and
integration) and reports line and branch coverage of `src/` and `include/`
(`tools/coverage_report.py`, on gcov's JSON output).

| | Lines | Branches |
| --- | --- | --- |
| Before this review | 1,379 of 1,445 (95.4%) | 1,467 of 2,439 (60.1%) |
| After the tests below, final code | 1,465 of 1,505 (97.3%) | 1,522 of 2,455 (62.0%) |

Branch coverage counts every outcome gcov sees, including the exception and allocation paths the
compiler adds, so it is a pointer to code worth reading, not a target. The uncovered lines fell into
three groups.

**Edge cases no test reached, now tested** (11 tests):

| Edge case | Test |
| --- | --- |
| a full lease table: keys it cannot hold take the exact path, and every check is answered | `LeasedCore.KeysAFullLeaseTableCannotHoldTakeTheExactPath` |
| a 65th distinct plan size of leased quota: the table of 64 leased rules is full, the exact path answers with the plan's policy | `LeasedQuotaTest.A65thDistinctPlanSizeTakesTheExactPath` |
| a batch that does not fit what is left of the in-flight limit: `RESOURCE_EXHAUSTED`, no entry reaches the engine | `AdmissionTest.ABatchThatDoesNotFitWhatIsLeftIsRefusedAtOnce` |
| a token return while the breaker is open: answered at once, Redis not asked | `ResilientBackendTest.AnOpenBreakerAnswersTokenReturnsAtOnce` |
| a timed-out plan read whose own timeout opened the breaker: the retry is not sent | `ResilientRetry.ATimedOutPlanReadIsNotRetriedOnceItsBreakerOpened` |
| a killed plan-change subscription comes back and asks for a resync; later messages arrive | `PlanListenerIT.AKilledSubscriptionComesBackAndResyncs` (integration) |
| usage events that cannot be written (`/dev/full`) are counted as dropped | `UsageLog.EventsThatCannotBeWrittenAreCountedAsDropped` |
| `ShardedBackend`, which had no test: one shard per key, keys over every shard, every call kind passed on | `ShardedBackend.*` (2 tests) |
| Redis metrics in the Prometheus text | `Metrics.RedisMetricsRenderEveryCallKindAndEveryKindOfError` |
| an unknown failure policy and a token bucket that never refills are rejected | `RuleConfig.RejectsAnUnknownFailurePolicyAndABucketThatNeverRefills` |

**Defensive code left untested on purpose:**
- malformed script replies (a reply of the wrong length), which the shipped scripts never produce;
- `RedisBackend`'s default `Return`, which every backend overrides;
- two retry paths for a lost connection inside the Redis client, which the fault tests exercise at
  run time (`faults_2026-10-23.md`).

**Races, reachable only between threads:**
- `LeasedCore::Park`'s second look at the lease, when a refill landed between the miss and the lock.
- The lock-free deadline queue's branches for an attempt caught between the two steps of its push,
  an attempt added while the timer stops, and the timer's re-check before it sleeps. These are
  exercised by the 8-thread stress test under TSan (section 3), not deterministically.

One gcov run reported the shutdown wait loop of `ReturnLeases` as never executed. The final run
reports it covered, and `ReturnLeasesWaitsForARefillInFlight` fails when the loop is removed (0
tokens returned instead of 9), so that report was wrong.

## 3. Locks on busy paths

Every `std::mutex` in `src/` was classified by the path it sits on.

| Mutex | Path |
| --- | --- |
| lease (`Lease::mu`) | lease miss: parking and refills only; a hit is one CAS |
| deny-cache shard | only a shard that holds a denial (one atomic load otherwise); a flood ran at the server's full rate (`flood_cardinality_2026-10-22.md`) |
| plan-cache shard | a miss, or a thread's first read; a hit reads the thread's own copy |
| usage log, check metrics, engines list, plan listener, batcher | waking a thread, reloads, rendering, off by default |
| **`ResilientBackend::mu_`** | **every Redis attempt, since the deadline of the fault test** |

The last one was new, so it was measured. `bench/bench_resilient` times one call through
`ResilientBackend` over a fake Redis that answers inline. The mutex-guarded deadline queue made the
call 5 times slower on one thread, and serialised the threads:

| Per call, median of 5 | No deadline | Deadline, mutex queue | Deadline, lock-free queue |
| --- | --- | --- | --- |
| 1 thread | 83.9 ns | 447.7 ns | **214.0 ns** |
| 4 threads | 39.1 M calls/s | 2.6 M/s | **13.7 M/s** |
| 12 threads | 57.9 M calls/s | 2.0 M/s | **21.9 M/s** (11×) |

The queue is now Vyukov's intrusive multi-producer, single-consumer queue:
- **Adding:** one atomic exchange; only the timer thread takes attempts out.
- **Allocation:** each attempt is one allocation that holds its own answer, where there were four.
  That also halved the path without a deadline, to 43 ns.
- **Testing:** a stress test has 8 threads send 24,000 attempts that Redis answers at once, later from
  another thread, or never. Every attempt must be answered exactly once. It passed 10 of 10 runs under
  TSan (`ResilientDeadline.ManyThreadsEveryAttemptAnsweredExactlyOnce`).

In the whole server the difference is within noise, because each check waits for Redis anyway.
Medians of 5 alternating runs:

| Workload | No deadline | Mutex queue | Lock-free queue |
| --- | --- | --- | --- |
| customer rule | 36,230 checks/s | 35,497 | 36,262 |
| exact daily quota | 41,631 | 41,698 | 41,541 |

The 3 to 5% cost reported for the deadline earlier the same day (`faults_2026-10-23.md`) was within
this noise, so it is corrected there.

## 4. Speed-ups found by profiling

`perf record -e cpu-clock` on the server under the customer rule (36,000 checks/s) gave a flat
profile:
- allocation (`malloc`, `free`, `operator new`, `gpr_malloc`) about 16%;
- abseil's mutex deadlock detection: `GraphCycles::GetId` 2.7%, plus `Mutex::Lock` and spin time,
  called from gRPC's own threads.

Ubuntu's abseil library checks every lock for deadlock cycles. A server needs no such check, so
`main.cc` now switches it off (`absl::SetMutexDeadlockDetectionMode(kIgnore)`). Medians of 5
alternating runs, closed loop (12 connections × 16 in flight):

| Workload | Detection on | Detection off | Change |
| --- | --- | --- | --- |
| leased rule (`api_default`) | 72,097 checks/s (71,004 to 73,319), p99 6.05 ms | **89,743** (87,001 to 92,879), p99 4.28 ms | **+24.5%** |
| customer rule | 36,469 (34,544 to 36,777), p99 12.6 ms | **41,654** (38,705 to 41,968), p99 11.1 ms | **+14.2%** |
| gRPC alone (`allow_all`) | 74,733 (71,977 to 77,413) | **91,154** (90,640 to 92,890) | +22.0% |

The ranges do not overlap. One line made the largest single gain of the project: the leased rule
now runs at 98% of gRPC with no limiter.

**jemalloc** (installed, used with `LD_PRELOAD`, no code change), on the new build:

| Workload | glibc malloc | jemalloc |
| --- | --- | --- |
| leased rule | 89,776 checks/s | 90,370 (+0.7%, noise) |
| customer rule | 41,556 | 43,105 (+3.7%, ranges apart) |

The customer path allocates per Redis call and gains a little. The lease path barely allocates and
gains nothing. jemalloc is a deployment option (`LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2`),
not a requirement.

## 5. Final state

- **Tests:** 236 pass in release, ASan and TSan (`scripts/test.sh`). The TSan run now starts the
  tests without address randomisation (`setarch -R`), because TSan sometimes cannot start under the
  kernel's randomisation ("FATAL: ThreadSanitizer: unexpected memory mapping").
- **Coverage:** 97.3% of lines.
  - 100% of: the rules parser, the composite engine, the service, the plan cache, the plan listener,
    the deny cache, the lease table, the batcher and the sharded backend.
  - What is left: the defensive and race paths named in section 2, and the Redis client wrapper's
    error paths (87%), which the fault tests exercise at run time.
- **Speed:** one limiter instance on this laptop:
  - the leased rule went from 72,097 to 89,743 checks/s (p99 6.05 to 4.28 ms);
  - the customer rule went from 36,469 to 41,654 checks/s, or 43,105 with jemalloc.

Raw output: `results/2026-10-23_resilient_bench/`, `results/2026-10-23_speedups/` (git-ignored).
