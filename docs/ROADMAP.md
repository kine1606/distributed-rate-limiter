# Roadmap: what to build, week by week

This is the working checklist for the thesis (Oct 12 to Dec 15, 2026). Every task links to the
module doc that holds its skeleton and hints. Tick a box only when the code is merged and its test
passes. The full plan, with scope, risks and the evaluation design, is in the shared planning doc;
this file is the version you work from every day.

How to use it: open the current week, pick the first unticked box, open the linked module doc,
and follow its "Build it in this order" section. Each module doc ends with a "Done when" list,
which is the definition of finished for that box.

Status legend: `[x]` done, `[ ]` to do, `(bonus)` drop first if late.

## Module map

| Module | Doc | Built in | Flow steps (Quota Service Flow page) |
| --- | --- | --- | --- |
| API contract | [proto/README.md](../proto/README.md) | Week 1 | A3, A13 |
| Lua scripts | [lua/README.md](../lua/README.md) | Weeks 1, 4, 5 | A10, B5 to B7 |
| Redis client and batcher | [src/redis/README.md](../src/redis/README.md) | Weeks 1 to 3 | A9 |
| Plans | [src/plan/README.md](../src/plan/README.md) | Weeks 2, 4 | A4, B1, B5, B9 |
| Engines (strict, composite) | [src/core/README.md](../src/core/README.md) | Weeks 2 to 4 | A6, A11 |
| gRPC server | [src/server/README.md](../src/server/README.md) | Weeks 2, 4, 5 | A4, A13 |
| Lease fast path | [src/lease/README.md](../src/lease/README.md) | Week 3 | A5, A7, A8 |
| Usage events | [src/usage/README.md](../src/usage/README.md) | Week 4 | A13, B8 |
| Reliability | [src/reliability/README.md](../src/reliability/README.md) | Week 5 | A12 |
| Metrics and logs | [src/metrics/README.md](../src/metrics/README.md) | Week 5 | A13 |
| Config and reload | [src/config/README.md](../src/config/README.md) | Weeks 2, 5 | A4 |
| Admin tools | [tools/README.md](../tools/README.md) | Weeks 2, 4 | B1, B5, B6, B9 |
| Tests | [tests/README.md](../tests/README.md) | every week | |
| Benchmarks and load generator | [bench/README.md](../bench/README.md) | Weeks 2, 3, 7, 8 | |
| Docker environment | [docker/README.md](../docker/README.md) | Weeks 1, 5, 6 | |
| Customer plan phase (Part B) | [CUSTOMER_PLANS.md](CUSTOMER_PLANS.md) | Weeks 2, 4, 5 | B1 to B9 |
| Design document | [DESIGN.md](DESIGN.md) | grows every week | |
| Evaluation | [EVALUATION.md](EVALUATION.md) | Weeks 6 to 8 | |
| Thesis chapters | [thesis/OUTLINE.md](thesis/OUTLINE.md) | 30 min every day | |

## Week 1 (Oct 12 to 18): Redis, Lua and the API contract

The goal of this week is a trustworthy storage layer and an agreed API. Everything later builds on
these two, so they get tested hardest.

- [x] Redis client spike: async cluster, pipelined `EVALSHA`, connection pool. Decide and write the
      justification. See [src/redis/README.md](../src/redis/README.md#week-1-the-client-spike).
- [x] Bring up the 3-master Redis Cluster and check slot routing with hash-tagged keys, then add
      the development profile so WSL can reach it on ports 7001 to 7003.
      See [docker/README.md](../docker/README.md).
- [x] `lua/quota.lua`: daily quota, UTC day from Redis `TIME`, stores `used`.
- [x] `lua/token_bucket.lua`: burst bucket with a `requested` argument.
- [x] `tests/lua_scripts_test.cc`: 13 cases against real Redis (rollover, upgrade, downgrade,
      cost greater than limit, expiry, concurrency).
- [x] Run the Lua tests on your machine against Docker Redis (`scripts/test.sh release`).
- [x] Proto v1: your redesign, then review. See [proto/README.md](../proto/README.md).
      Reviewed and tagged `proto-v1` on 2026-10-18.
- [x] Thesis: start Chapter 2 notes on token bucket, Redis Lua atomicity and Redis Cluster slots.
      Done as the Chapter 2 draft (`docs/thesis/ch2_background.md`).

Gate: both scripts pass every edge case on your machine; proto v1 agreed.
Gate status: met.

## Week 2 (Oct 19 to 25): Strict engine end to end

The goal is the simplest correct system: every check goes to Redis. It is slow, but it is the
baseline that every later optimisation is compared against, and it proves the quota is exact.

- [x] `RedisBackend` implementation with `SCRIPT LOAD` at startup and `NOSCRIPT` fallback.
      (SHA1 computed locally instead of `SCRIPT LOAD`; `EVAL` on `NOSCRIPT`.)
      See [src/redis/README.md](../src/redis/README.md#week-2-the-real-backend).
- [x] `PlanStore` that reads `{cust:N}:plan` (no cache yet). See [src/plan/README.md](../src/plan/README.md).
- [x] `StrictCore` behind `RateLimiterCore`; replace `AllowAllCore` in `main.cc`.
      See [src/core/README.md](../src/core/README.md).
- [x] Rule config extended with plan-aware fields. See [src/config/README.md](../src/config/README.md).
- [x] `tools/seed_plans.sh` writes free, pay and enterprise customers. See [tools/README.md](../tools/README.md).
- [x] Integration test: a free customer is stopped at exactly 200 through gRPC.
- [x] Baseline benchmark with `ghz` on 1 instance; raw output in `results/`.
      Strict quota: 25,301 checks/s median of 5 at c=1,024 (p99 85 ms); 10,024 at p99 10 ms (c=64).
      See [bench/README.md](../bench/README.md#week-2-baseline).
- [x] Thesis: Chapter 1 and Chapter 2 drafts (`docs/thesis/ch1_introduction.md`,
      `ch2_background.md`, sources in `references.md`; every entry checked against its original
      on 2026-10-18).

Gate: a free customer is stopped at exactly 200 through gRPC; baseline numbers saved.
Gate status: met.

Status on Oct 17: Weeks 1 and 2 are done about a week early (proto v1 review and the Chapter 1
and 2 drafts still open). Strict baseline: 25,301 checks/s median at c=1,024 (p99 85 ms); 10,024
checks/s at p99 10 ms (c=64). Start Week 3 now and spend the spare week on the Should items below;
the Nov 22 freeze does not move. Each week lists what to cut first if it runs late.

## Week 3 (Oct 26 to Nov 1): Lease fast path for the burst rule

Goal: burst checks served from local memory, Redis sees only batched refills, and a harness that
proves the lease never breaks its bound. About 16 h.

- [x] Lease table (4 h): sharded map from interned key id to a 64-byte-aligned lease; `TryTake` is a
      CAS loop. Interning turns the key into a `uint64` once, so a hit allocates nothing.
      See [src/lease/README.md](../src/lease/README.md).
      Built as an interned 64-bit id stored in each lease: lookups compare integers, the key string
      only on a match; a hit allocates nothing. +16 to 43% on lookups over 10,000 keys
      (`results/2026-10-18_lease_microbench/NOTES.md`). The table is lock-free, so it has no shards.
- [x] Single-flight refill with early refill at 30% of the last chunk (2 h). 500 concurrent
      misses cause one refill (`ConcurrentMissesShareOneRefillAndAllGetItsAnswer`).
- [x] Adaptive chunk size (2 h): double when the lease empties within one refill round trip, halve
      when tokens sit unused, never above the cap. Log changes for a chunk-vs-demand plot.
      Done (`src/lease/chunk_policy.h`; log with `RL_CHUNK_LOG=<file>`). Refills now top up to the chunk.
- [x] Adaptive batcher (3 h, Should): flush at once when idle, batch under load. Compare with a fixed
      200 µs timer (p50 at light load, throughput at heavy load).
      See [src/redis/README.md](../src/redis/README.md#week-3-the-batcher).
      Done (`src/redis/batcher.{h,cc}`, `RL_BATCHER=timer|adaptive`), off by default: the timer costs
      +130 us p50 at light load, and neither mode gains throughput, because redis-plus-plus already
      writes concurrent calls together (`results/2026-10-18_batcher/NOTES.md`).
- [x] Deterministic test harness (2 h): fake Redis with a fake clock replying in seeded random order;
      thousands of seeds assert admitted <= limit + N × C; a failing seed replays exactly.
      Done (`tests/lease_harness_test.cc`): it found two ways the protocol broke the bound, both fixed;
      100,000 seeds now pass (DESIGN.md section 6).
- [x] Microbenchmark ablation (2 h): `alignas(64)` on/off, CAS vs `fetch_sub`, 1/16/64 shards, at
      1/4/16 threads, one hot key and 10,000 keys. False-sharing figure for Chapter 4.
      Done (`bench/bench_lease.cc`): false sharing costs 8.7x at 4 threads and 14x at 16; one hot key
      stays near 30 M/s at any thread count; `fetch_sub` is 2.5x faster than the CAS loop but not
      adopted (spurious denials). Shard counts do not apply to the lock-free table.
- [x] `LeasedCore` for burst rules; TSan on every lease test from the first commit.
- [x] Thesis: lease protocol notes and the accuracy-bound derivation draft (Chapter 3).
      `docs/thesis/ch3_design.md` section 3.5: protocol, theorem and proof, harness evidence, alternatives.

Gate: lease hit ratio >= 95% under the Week 2 load; 500 concurrent misses cause one refill; lease
mode >= 4× the 25,301 checks/s strict baseline on the same ghz command, p99 < 2 ms at 10,000/s offered.
Cut first: adaptive batcher.

Gate status: partly met.
Gate result (2026-10-18, `bench/week3_gate.sh`, `results/2026-10-18_week3_gate/NOTES.md`): hit ratio
99.06% (met); 500 misses cause one refill (met); lease 6.3x strict at equal load on the C++ load
generator and 99% of gRPC alone, but 1.44x on the ghz command, which is client-bound, and 2.8x the
25,301 baseline in absolute terms, which is above what gRPC alone reaches on this laptop (71,364/s);
p99 at 10,000/s is 2.20 ms against 1.89 ms for gRPC alone (not met by 0.2 ms; the laptop floor is the
limit). In the measured 30 s of those p99 runs at most 14 of 300,000 checks waited for Redis (the
server counts 10,005 to 10,014 waits and one refill per key per run, 10,000 of the waits being the
first touch of each key during the warm-up), so the 0.3 ms gap is the server's CPU path and run-to-run noise (leased 1.81 to 4.12 ms,
gRPC alone 1.78 to 2.06 ms over five runs), not Redis. The absolute 4x and the 2 ms p99 need
separate machines (Week 8 bonus run).

## Week 4 (Nov 2 to 8): Business rules and leased quota for large plans

Goal: quota-service behaviour, plus the change that makes the 1M checks/s argument hold. About 16.5 h.

- [x] Overage policies and request ID in `quota.lua` (4 h); Week 1 tests still pass unchanged.
      See [lua/README.md](../lua/README.md#week-4-overage-policies).
      Done: `hard`, `grace` (free, answers `OK`), `cap` (billed, answers `OVERAGE`); a retried request
      ID gets the stored answer and is counted once; `return_lease.lua` built early for the burst refund.
- [x] Local deny cache (1.5 h). See [src/lease/README.md](../src/lease/README.md#deny-cache-week-4).
      Done (`src/lease/deny_cache.h`): ends at the Redis reset; a changed plan lifts it at once.
- [x] `CompositeCore` (3 h): suspended, deny cache, burst, quota, overage; give burst back on a quota
      deny. See [src/core/README.md](../src/core/README.md#week-4-compositecore).
      Done: rule `customer` in `config/rules.yaml`, one plan read per call, a test per branch; request
      2,001 gives three answers on three policies through gRPC (first part of the gate).
- [x] `PlanCache` with Pub/Sub invalidation and a 5 s TTL (2 h). See [src/plan/README.md](../src/plan/README.md).
      Done: the cache lives in `PlanStore` (single flight; a read racing a change is sent again);
      `PlanListener` subscribes to `plan-changes`. An upgrade reached two instances 22 ms after
      `rlctl` started (`tests/plan_change_it.cc`, steps B5 and B9). On the strict baseline command
      the cache alone gives 16,738 to 19,480 checks/s (median of 5 each, same session; the Week 2
      binary measured 15,851 that day, so compare within a session only).
- [x] Leased quota for large plans (3 h, Should): `quota_mode: auto`, strict below a daily limit of
      about 100,000, leased above with chunk <= 0.1% of the daily limit.
      Done: `CompositeCore` leases through `LeasedCore::CheckLease`; a lease takes only units inside
      the plan, and overage and request IDs take the exact path. Four instances on one customer of
      100,000 a day admitted exactly 100,000 units with 1,253 quota calls for 12,048 checks
      (`tests/leased_quota_it.cc`). Also lock-free on the customer path (plan cache hits, empty deny
      shards, usage rings): one hot customer on 16 threads, plan hits 4.1 to 263.8 M/s
      (`results/2026-10-18_customer_microbench/NOTES.md`).
- [x] Usage events, JSON lines per customer per minute (2 h). See [src/usage/README.md](../src/usage/README.md).
      Done: `RL_USAGE_FILE`; a retried request ID is recorded once (`quota.lua` marks replays);
      `tools/usage_report.py` totals matched Redis `used` exactly in a smoke run.
- [x] `tools/rlctl` (1 h). See [tools/README.md](../tools/README.md).
      Done: create, upgrade, downgrade, suspend, show; the plan catalogue moved to `tools/plans.sh`.
- [x] Thesis: quota and overage section of Chapter 3.
      Done: `docs/thesis/ch3_design.md` section 3.4 (decision order, Table 3.y with request 2,001,
      retries, plan changes, leased quota).

Gate: request 2,001 gives three outcomes on three policies; an enterprise customer on leased quota
stays within limit + N × C; every step B1 to B9 in [CUSTOMER_PLANS.md](CUSTOMER_PLANS.md) has a test
or a "described, not built" note. Cut first: leased quota (becomes future work item one).

Gate status: met.
Gate result (2026-10-19): request 2,001 gives three outcomes through gRPC
(`GrpcQuotaIT.Request2001GivesThreeAnswersOnThreePolicies`); an enterprise customer on leased quota
stays within limit + N × C (four instances, 100,000 a day: exactly 100,000 admitted,
`LeasedQuotaIT.EnterpriseCustomerStaysWithinLimitPlusNTimesC`); every step B1 to B9 has a test or a
"described, not built" note ([CUSTOMER_PLANS.md](CUSTOMER_PLANS.md)).

## Week 5 (Nov 9 to 15): Reliability, observability and honest measurement

About 16 h.

- [x] Timeouts (5 to 10 ms), circuit breaker, retries only on timeouts with jitter (3 h).
      Done (`src/reliability/`): breaker (20 in a row, one probe after 500 ms), retries of timeouts
      that cannot loosen a limit. 10 ms raised false timeouts under load, so the default is 50 ms,
      with four Redis clients (exact path 8,896 to 36,283 checks/s). See `docs/thesis/evidence/reliability_2026-10-21.md`.
      See [src/reliability/README.md](../src/reliability/README.md).
- [x] Failure policy per plan after leases run out; fail-open admissions marked `unverified` (1.5 h).
      Done: the plan's `failure` field decides for customer checks; stale-if-error plans. Master
      stopped 3.3 s: Pay 59 of 59 `UNVERIFIED`, Free 59 of 59 `UNAVAILABLE`.
- [x] Bounded queues and `RESOURCE_EXHAUSTED`; graceful shutdown returns leases (2.5 h).
      Done: one in-flight bound in the service (`RL_MAX_IN_FLIGHT`, 10,000) bounds every queue behind
      it, no measurable cost; SIGTERM drains, returns leased tokens (72,194 from 1,000 leases in 7 ms).
- [ ] Health readiness follows the breaker; SIGHUP reload (2 h, Should).
- [x] Prometheus metrics (2 h). See [src/metrics/README.md](../src/metrics/README.md).
      Done: own text exposition (prometheus-cpp is not installed), striped lock-free counters,
      `/metrics` on `RL_METRICS_ADDR` (127.0.0.1:9100); Prometheus in `docker-compose.yml`. Overhead
      not measurable: leased 77,377 checks/s on, 75,394 off (medians of 3).
- [x] Open-loop load generator (3 h): fixed arrival rate, latency from scheduled send time (avoids
      coordinated omission). Try `ghz --rps` first. See [bench/loadgen/README.md](../bench/loadgen/README.md).
      Done: `bench/loadgen` with a `rate` argument (`ghz --rps` times calls from their send, so it
      has the same blind spot). Server frozen 0.5 s at 10,000/s: open-loop p99 480.8 ms, closed-loop
      5.8 ms. Leased rule open loop: p99 1.02 ms at 10,000/s, 3.76 ms at 40,000/s; saturates by 50,000.
- [ ] Thesis: Chapter 3 draft.

Gate: kill a Redis master under load and observe the documented behaviour per plan; latency at a
fixed rate measured open-loop. Cut first: SIGHUP reload.

Gate status: met.
Gate result (2026-10-21, `docs/thesis/evidence/week5_gate_2026-10-21.md`): with a master killed under load, a third of the customers (the
dead master's slots) got their plan's policy for 4 to 5 s, Free `UNAVAILABLE` and Pay `UNVERIFIED`,
leases serving for 2 s first, no call failed (3 runs). On the way the test found and fixed a
cluster-wide breaker (now per slot group), scripts not following a failover (now re-routed once) and
gRPC collapsing under open-loop overload (now 100 streams per connection). Open loop, leased rule:
p99 0.83 ms at 10,000/s, 1.80 ms at 20,000/s; queueing, not failing, past about 25,000/s.

## Week 6 (Nov 16 to 22): Correctness evidence and freeze

About 16.5 h.

- [ ] TSan and ASan clean (2 h). See [tests/README.md](../tests/README.md#sanitizers).
- [ ] Property-based tests (3 h, Should): random histories of checks, retries, plan changes and
      rollovers against a simple reference model; seeded.
- [ ] Quota accuracy test, 4 instances, 10% retries, 20 runs (2 h).
- [ ] Bound model vs measurement (3 h): Python simulation predicts over-admission; burst accuracy
      runs at chunk caps 1%, 5%, 10%; plot predicted, measured and worst case.
- [ ] Flood and cardinality (1M keys) tests (2 h).
- [ ] Fault tests: kill master, 20 ms latency (toxiproxy), dropped connections (3 h).
- [ ] Noisy-neighbour test (1.5 h, Should): other tenants' p99 with and without one flooding tenant.
- [ ] Nov 22: feature freeze, tag `v1.0`. See [EVALUATION.md](EVALUATION.md).

Gate: all tests green; measured over-admission at or below the derived bound.
Cut first: noisy neighbour, then connection-drop faults.

## Week 7 (Nov 23 to 29): Benchmarks, ablation and measured tuning

About 16 h, much of it unattended runs. See [bench/README.md](../bench/README.md).

- [ ] Benchmark matrix (5 h): engines × 1/2/4 instances × chunk caps × uniform/Zipf/hot keys,
      open-loop, 5 runs, median with min and max, cores pinned with `taskset`.
- [ ] Ablation study (3 h): switch off lease, batcher, early refill, adaptive chunk, deny cache,
      plan cache, alignment one at a time; throughput and p99 delta table.
- [ ] Profile and fix the top one or two bottlenecks (4 h): only what the flamegraph shows, e.g.
      per-thread sub-leases for a hot key, protobuf arenas or pooled callbacks, gRPC thread count.
- [ ] Build-level wins, each measured (2 h, Should): LTO, PGO, mimalloc or jemalloc via LD_PRELOAD.
- [ ] (bonus) Redis Functions vs `EVALSHA`, same benchmark.
- [ ] Thesis: Chapter 4.

Gate: matrix, ablation table and flamegraph in `results/`; every Chapter 5 table generated by a script.
Cut first: build-level wins, then Redis Functions.

## Week 8 (Nov 30 to Dec 6): Models, final numbers and the evaluation chapter

- [ ] Universal Scalability Law fit over instances and threads (2 h); project instances for 1M/s.
- [ ] Little's law cross-check of in-flight = throughput × latency (0.5 h).
- [ ] Capacity and cost model with cited, dated prices (2 h, Should).
- [ ] Extrapolation to 1M checks/s with every assumption written out (1 h).
- [ ] (bonus) One AWS run on separate machines, budget alarm first.
- [ ] Thesis: Chapter 5 (8 h); `DESIGN.md` and `README.md` finished.

Gate: full thesis draft by Dec 6. Cut first: AWS run, then prices in the cost model.

## Week 9 (Dec 7 to 15): Review and delivery

- [ ] Dec 7: full draft to advisor; apply feedback.
- [ ] Chapter 6, proofreading, references, figures, faculty format.
- [ ] Reproducibility check on a clean clone: one command rebuilds, tests and regenerates every table.
- [ ] Slides, two-minute demo, backup video; (bonus) Spring Boot demo gateway.
      See [examples/gateway-springboot/README.md](../examples/gateway-springboot/README.md).
- [ ] Mock defense with the 20 questions in `docs/thesis/Thesis_Defense_Brief.docx`.
- [ ] Dec 15: submit.

## If you fall behind

Drop work in this order and write the dropped item into Chapter 6 as future work: AWS run, demo
gateway, Redis Functions, build-level wins, SIGHUP reload, noisy-neighbour test, fault tests beyond
killing one node. Never drop the quota accuracy test or
the benchmark report, because they carry the thesis claim.
