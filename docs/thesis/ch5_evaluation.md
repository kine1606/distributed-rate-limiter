# Chapter 5. Evaluation

> Draft, started Week 7 (2026-10-24) from the Week 6 evidence. Target 12 to 14 pages. The benchmark
> matrix and the ablation are marked **[Pending: Week 7]**; the final extrapolation **[Pending: Week 8]**.

This chapter tests the two halves of the thesis claim separately, as Chapter 1 set out. First
correctness: quotas are exact under concurrency, retries, plan changes and failures (5.2 to 5.4).
Then cost: what the leased design buys in throughput and latency, and what it costs in accuracy and
isolation (5.5 to 5.10). Every number comes from a script in the repository, and every table names
its evidence file in `docs/thesis/evidence/`.

## 5.1 Setup

| Item | Value |
| --- | --- |
| Machine | laptop, Intel Core i5-12450H (8 cores, 12 hardware threads), 7 GB of RAM given to WSL2 |
| System | Ubuntu 24.04 in WSL2 (kernel 6.6.114.1-microsoft-standard-WSL2), gcc 13.3, `-O3 -march=native` |
| Redis | 7.4.11 in Docker 28.5.1: a development cluster of 3 masters; for fault tests, 3 masters with one replica each; no persistence |
| Limiter | the `release` build at the commit named in each evidence file, defaults unless stated |
| Load | `bench/loadgen` (C++, gRPC callback API): closed loop at a fixed number of calls in flight, or open loop at a fixed rate with latency from each call's scheduled time; Python clients for the accuracy tests |

The load generator, the limiter and Redis share one laptop. That bounds every throughput figure from
below: on separate machines each would have its own cores. It also makes the noise large, often ±10%
between runs, so the method follows from it.
- **Repetition:** every cell runs at least 3 times, usually 5, and 20 for quota accuracy; tables give the
  median with the range.
- **Comparisons:** builds or settings are compared in alternating runs within one session, never
  against a number from another day.
- **Isolation:** for the matrix, the servers and load generators are pinned to separate cores (`taskset`).
- **Extrapolation:** cluster-wide figures are labelled as estimates (5.12), never as measurements.

## 5.2 Quota accuracy (evaluation test 1)

**Setup.** `tests/eval/quota_accuracy.sh`:
- **Instances:** four limiter instances.
- **Customers:** one Free (200 a day) and one Pay (2,000 a day with a 10% grace, so 2,200). Their burst
  limits are raised so that the quota binds.
- **Load:** 400 and 4,400 calls of the `customer` rule, each with a request id, 64 in flight, spread
  over the instances.
- **Duplicates:** a tenth of the requests are sent twice at the same moment to two different
  instances. That is the hardest case for deduplication, because neither instance can know about the
  other's copy.
- **Repetition:** 20 runs.

**Result.** All 20 of 20 runs were exact. In every run three independent counts each equal the
allowance:
- the request ids that got an allowed answer;
- the counter in Redis;
- the usage events of the four instances.

The 96,000 requests included 9,608 sent twice at once, and none was counted twice. No request id got
an allowed answer from one instance and a denial from the other, and no call failed
(`quota_accuracy_2026-10-22.md`).

**Property tests.** Random histories go through the real engine on a transcription of the scripts,
under a fake clock. They mix checks, retries, plan changes, outages, clock jumps and UTC rollovers.
Each answer and usage event is compared with a reference model of Chapter 3's rules. 20,000 histories
per configuration gave 22.6 million checks, every one equal to the model. A second property bounds
leased quota for large plans. Over 80,490 simulated days, Redis never granted beyond the plan, and
what N instances admitted was what Redis granted, minus at most N × C units still held in leases.
The test catches bugs: ten planted in the engine one at a time were all caught
(`property_tests_2026-10-23.md`).

## 5.3 Plan changes, the reset and overage (evaluation tests 2 and 3)

- **Upgrades apply at once.** An upgrade written by the admin tool reached two instances 22 ms after
  the tool started (`PlanChangeIT`). The new limit applied on the next call and kept the day's usage.
- **A downgrade below today's usage** denies with `remaining = 0`, never a negative number
  (`QuotaDowngradeBelowUsageDeniesWithoutNegativeRemaining`).
- **One reset.** The day is derived from the Redis clock. The script tests cross a UTC midnight, and
  the property histories crossed 34,425 rollovers, including steps landing within ±1.5 s of the reset.
  The local denial ends exactly at the reset.
- **Request 2,001 gets three answers** under the three overage policies, through gRPC
  (`Request2001GivesThreeAnswersOnThreePolicies`):

  | Policy | Answer |
  | --- | --- |
  | hard | denied |
  | grace | allowed, free |
  | cap | allowed, billed as overage |

  The property histories produced 133,772 overage answers, all as the model predicted.

## 5.4 The burst accuracy bound and its model (evaluation tests 4 and 12)

**Setup.** `tests/eval/burst_accuracy.sh`:
- **Instances and key:** four instances serve one hot key of a leased bucket, 1,000 tokens refilled at
  100 a second.
- **Chunk caps:** 1%, 5% and 10% of the limit, so the worst case N × C is 40, 200 and 400 tokens.
- **Demand:** two shapes, at 2,000 calls a second in all:
  - a burst after an idle period, the worst case the proof allows;
  - steady overload.
- **Measure:** the over-admission is the largest excess, over any window, of what was admitted beyond
  what any exact bucket could admit.
- **Model:** a simulation of the protocol, written independently of the engine, replays the same
  calls.

![Over-admission against the chunk cap](figures/burst_bound.png)

*Figure 5.1: Over-admission against the chunk cap: measured (median of 5 runs), predicted, and the
worst case N × C (`burst_bound_2026-10-22.md`).*

| Cap | Worst case N × C | After idle: measured | model | Steady overload: measured |
| --- | --- | --- | --- | --- |
| 1% | 40 | 34.7 | 34.4 | −0.1 |
| 5% | 200 | 189.0 | 188.8 | −1.1 |
| 10% | 400 | 381.4 | 381.4 | −1.5 |

- **The bound holds and is tight.** Every run stayed below N × C. After an idle period the excess
  approaches it: each instance held a full lease, the bucket had refilled, and part of the refill
  landed within the window.
- **Under steady overload, leases cost no accuracy.** Over a whole run the leases admitted slightly
  fewer calls than an exact bucket would have.
- **The model predicts the system.** Measured and predicted values agree within 0.3 tokens, and the
  admitted counts are identical in 29 of 30 runs.

## 5.5 Floods and many customers (evaluation tests 5 and 6)

**A flood** (`flood_cardinality_2026-10-22.md`). A Free customer out of quota retried flat out, and
the instance denied it 74,881 times a second, its full rate for any check. Those denials cost Redis
2 calls in 10 seconds, both of them plan re-reads, because the deny cache answers locally. At a fixed
10,000 calls a second a denial took 0.24 ms at the median and 0.61 ms at p99.

**A million customers.**

| | 10,000 customers | 1,000,000 customers |
| --- | --- | --- |
| customer rule | 38,201 checks/s | 28,365 (−26%), 0.89 plan reads per check |
| leased rule | 72,902 checks/s | 47,320 (−35%), 0.62 refills per check, 224 MB of leases |

Redis memory per active customer is about 420 bytes: a plan of 171 bytes, plus a daily counter and a
burst bucket of 112 each by `MEMORY USAGE`. Uniform keys over a million customers are the hard case
for both caches: a customer's next call comes after its plan entry has expired, and a lease is used
about once. Skewed traffic, measured in 5.6, keeps both warm.

## 5.6 Strict against leased, and scaling (evaluation tests 7 and 8)

**Setup.** `bench/run_matrix.sh` covers:
- **Engines:** no limiter, strict, leased at three chunk caps, and the customer check.
- **Scale:** 1, 2 and 4 instances.
- **Keys:** uniform over a million keys, Zipf over a million keys, and one hot key.
- **Load:** closed loop at 192 calls in flight in all, 5 runs per cell; servers and load generators on
  separate cores.

The tables are generated by `tools/matrix_tables.py` from the summary **[Pending: Week 7, matrix
running]**.

Single-instance results from earlier sessions, unpinned, are in `verification_and_speedups_2026-10-23.md`:
the leased rule ran at 89,743 checks per second, 98% of gRPC with no limiter.

## 5.7 Faults (evaluation test 9)

`tests/eval/faults.sh` and `bench/fault_kill_master.sh` (`faults_2026-10-23.md`). Each fault is
measured before, during and after, with Free, Pay and leased streams. Their rates are 1,000, 5,000
and 10,000 checks a second.

| Fault | What each plan got | Recovery |
| --- | --- | --- |
| +20 ms on every master (below the 50 ms timeout) | nothing failed; customer checks p50 1.45 → 42.5 ms (two round trips); leased checks 0.61 → 0.64 ms | at once |
| +80 ms on one master | its third of the customers: Free `UNAVAILABLE`, Pay `UNVERIFIED`, at p99 under 4 ms; leases served until dry | within 1 s |
| one master silent (replies lost) | the same | before the next phase |
| every connection killed 5 times in 10 s | 2 to 18 calls followed the policy (those in flight); subscription back in 0.5 s | at once |
| a master killed, replica promoted | a third of the customers got their policy for 4 to 5 s | 4 to 5 s |

No call failed in any fault run. The test also found a fault in the design. The Redis client's
timeout is an idle timeout per connection, so before the fix a master 80 ms slow under load was never
timed out: checks succeeded after about 250 ms, with no fallback. Each Redis attempt now has its own
deadline.

## 5.8 A noisy neighbour (evaluation test 11)

Other tenants ran at 5,000 checks a second while one tenant sent 20,000 a second
(`noisy_neighbour_2026-10-23.md`).

| Noisy tenant | Others' p99 | Redis calls a second on its master |
| --- | --- | --- |
| none | 4.5 ms | 3,319 |
| over its daily quota | 14.4 ms | 3,319 |
| over its burst, within quota | 31.4 ms | 25,233 |

The others' answers stayed correct in every run.
- **Over quota:** the flood cost Redis nothing; the latency rise is shared CPU on the laptop.
- **Over burst:** each call ran the burst script, and that is the isolation gap the design leaves
  open (3.6.1).

## 5.9 Ablation (evaluation test 10)

`bench/ablation.sh` switches one mechanism off at a time on the workload where it should matter:
- the lease;
- early refill;
- the adaptive chunk;
- cache-line alignment;
- the batcher;
- the deny cache;
- the plan cache.

**[Pending: Week 7.]**

## 5.10 Profiling and speed-ups

A profile of the server under load (`perf`, 2026-10-23) found abseil's mutex deadlock detection
running in gRPC's own threads: 2.7% of the CPU in `GraphCycles::GetId` alone. Switching it off raised
throughput a lot:

| Workload | Before | After | Change |
| --- | --- | --- | --- |
| leased rule | 72,097 checks/s | 89,743 | +24.5% |
| customer rule | 36,469 | 41,654 | +14.2% |
| gRPC alone | 74,733 | 91,154 | +22.0% |

Making the per-attempt deadline queue lock-free took a Redis call through the resilient layer from
448 to 214 ns. jemalloc added 3.7% on the customer rule and nothing on the leased one. The flamegraph
of the final build is **[Pending: Week 7]**.

## 5.11 Extrapolation to 1,000,000 checks per second

Section 3.8 derives the estimate. It needs 12 to 50 instances for leased rules, depending on the
latency target, and 9 to 17 Redis masters for customer checks, from per-instance and per-master
measurements. The final figures, with the matrix's scaling data, are **[Pending: Week 8]**. They are
estimates, and labelled as such.

## 5.12 Discussion and threats to validity

**What the numbers support.**
- Quotas are exact: 20 of 20 adversarial runs on four instances, and 22.6 million checks equal to a
  model.
- The lease's over-admission is bounded by N × C. The bound is reached only after an idle period, and
  it is predicted by an independent model.
- A leased check costs 2% over gRPC with no limiter. Failures degrade per plan, as documented, without
  failed calls.

**What they do not support yet.**
- **Absolute throughput on server hardware.** The load generator shares the laptop.
- **Scaling beyond four instances.**
- **Behaviour under real network latency.** The 20 ms test is a proxy for it.

**Threats to validity.**
- **One laptop.** Client, limiter and Redis compete for the same cores, so throughput is a lower bound
  and the noise is large. Comparisons are therefore made in alternating runs.
- **Docker networking on loopback.** Real round trips are longer, and they hurt the exact path more
  than the leased one (5.7).
- **The fake Redis of the property tests** is a transcription of the scripts. The scripts themselves
  are tested against a real Redis.
- **Uniform and Zipf keys are models of traffic,** not traces of real traffic.
