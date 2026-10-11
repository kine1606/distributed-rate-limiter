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

- [ ] Redis client spike: async cluster, pipelined `EVALSHA`, connection pool. Decide and write the
      justification. See [src/redis/README.md](../src/redis/README.md#week-1-the-client-spike).
- [ ] Bring up the 3-master Redis Cluster and check slot routing with hash-tagged keys, then add
      the development profile so WSL can reach it on ports 7001 to 7003.
      See [docker/README.md](../docker/README.md).
- [x] `lua/quota.lua`: daily quota, UTC day from Redis `TIME`, stores `used`.
- [x] `lua/token_bucket.lua`: burst bucket with a `requested` argument.
- [x] `tests/lua_scripts_test.cc`: 13 cases against real Redis (rollover, upgrade, downgrade,
      cost greater than limit, expiry, concurrency).
- [ ] Run the Lua tests on your machine against Docker Redis (`scripts/test.sh release`).
- [ ] Proto v1: your redesign, then review. See [proto/README.md](../proto/README.md).
- [ ] Thesis: start Chapter 2 notes on token bucket, Redis Lua atomicity and Redis Cluster slots.

Gate: both scripts pass every edge case on your machine; proto v1 agreed.

## Week 2 (Oct 19 to 25): Strict engine end to end

The goal is the simplest correct system: every check goes to Redis. It is slow, but it is the
baseline that every later optimisation is compared against, and it proves the quota is exact.

- [ ] `RedisBackend` implementation with `SCRIPT LOAD` at startup and `NOSCRIPT` fallback.
      See [src/redis/README.md](../src/redis/README.md#week-2-the-real-backend).
- [ ] `PlanStore` that reads `{cust:N}:plan` (no cache yet). See [src/plan/README.md](../src/plan/README.md).
- [ ] `StrictCore` behind `RateLimiterCore`; replace `AllowAllCore` in `main.cc`.
      See [src/core/README.md](../src/core/README.md).
- [ ] Rule config extended with plan-aware fields. See [src/config/README.md](../src/config/README.md).
- [ ] `tools/seed_plans.sh` writes free, pay and enterprise customers. See [tools/README.md](../tools/README.md).
- [ ] Integration test: a free customer is stopped at exactly 200 through gRPC.
- [ ] Baseline benchmark with `ghz` on 1 instance; raw output in `results/`.
      See [bench/README.md](../bench/README.md#week-2-baseline).
- [ ] Thesis: Chapter 1 and Chapter 2 drafts.

Gate: a free customer is stopped at exactly 200 through gRPC; baseline numbers saved.

## Week 3 (Oct 26 to Nov 1): Lease fast path for the burst rule

The goal is speed without losing correctness: burst checks are served from local memory, and
Redis only sees batched refills.

- [ ] Sharded, cache-line-aligned `LeaseTable`; atomic decrement on a hit.
      See [src/lease/README.md](../src/lease/README.md).
- [ ] Single-flight refill, low-watermark early refill, adaptive chunk size with a cap.
- [ ] Pipelining `Batcher` per Redis node (flush at max size or 100 to 500 µs).
      See [src/redis/README.md](../src/redis/README.md#week-3-the-batcher).
- [ ] `LeasedCore` for burst rules; strict path kept for quotas.
- [ ] Google Benchmark of the hit path; run TSan on the lease tests from now on.
- [ ] Thesis: notes for the accuracy-bound derivation (Chapter 3).

Gate: burst checks mostly served without Redis; lease mode clearly faster than strict mode.

## Week 4 (Nov 2 to 8): Business rules

The goal is the business behaviour that makes this a quota service, not just a rate limiter.

- [ ] Overage policies `hard`, `grace`, `overage_cap` in `quota.lua`; `OVERAGE` reason.
      See [lua/README.md](../lua/README.md#week-4-overage-policies).
- [ ] Request ID deduplication. See [lua/README.md](../lua/README.md#week-4-request-id).
- [ ] Local deny cache. See [src/lease/README.md](../src/lease/README.md#deny-cache-week-4).
- [ ] `CompositeCore`: quota (strict) plus burst (leased) for one API call.
      See [src/core/README.md](../src/core/README.md#week-4-compositecore).
- [ ] `PlanCache` with Pub/Sub invalidation. See [src/plan/README.md](../src/plan/README.md).
- [ ] Usage events to a log file or SQLite. See [src/usage/README.md](../src/usage/README.md).
- [ ] `tools/rlctl` for upgrade, downgrade, suspend. See [tools/README.md](../tools/README.md).

Gate: the same request 2,001 gives three different outcomes for three customers on three policies,
and every customer plan step B1 to B9 in [CUSTOMER_PLANS.md](CUSTOMER_PLANS.md) has a test or a
"described, not built" note.

## Week 5 (Nov 9 to 15): Reliability and observability

The goal is that the system behaves in a documented way when Redis is slow, missing or overloaded,
and that you can see what it is doing.

- [ ] Redis timeouts, circuit breaker, bounded retries with jitter.
      See [src/reliability/README.md](../src/reliability/README.md).
- [ ] Failure policy per plan (paid fail open, free fail closed) after leases run out.
- [ ] Bounded queues and `RESOURCE_EXHAUSTED`; graceful shutdown returns leased tokens.
- [ ] Health readiness tied to Redis; SIGHUP rule reload.
- [ ] Prometheus metrics. See [src/metrics/README.md](../src/metrics/README.md).
- [ ] Thesis: Chapter 3 (design) with the architecture diagram and accuracy bound.

Gate: kill Redis in Docker and observe the documented behaviour per plan.

## Week 6 (Nov 16 to 22): Correctness testing and freeze

- [ ] TSan and ASan runs clean. See [tests/README.md](../tests/README.md#sanitizers).
- [ ] Accuracy, flood, cardinality and fault tests. See [EVALUATION.md](EVALUATION.md).
- [ ] Nov 22: feature freeze, tag `v1.0`.

Gate: all tests green, no known data race.

## Week 7 (Nov 23 to 29): Benchmarks and tuning

- [ ] Benchmark matrix: strict vs lease, chunk sizes, 1, 2 and 4 instances, key distributions.
- [ ] `perf` profile and flamegraph; fix the top one or two bottlenecks, then re-measure.
- [ ] (bonus) Spring Boot demo gateway. See [examples/gateway-springboot/README.md](../examples/gateway-springboot/README.md).
- [ ] Thesis: Chapter 4 (implementation).

## Week 8 (Nov 30 to Dec 6): Final numbers and evaluation chapter

- [ ] (bonus) One AWS run on separate machines, budget alarm first.
- [ ] Extrapolation to 1M checks/s with every assumption written out.
- [ ] Thesis: Chapter 5 (evaluation); `DESIGN.md` and `README.md` finished.

Gate: full thesis draft by Dec 6.

## Week 9 (Dec 7 to 15): Review and delivery

- [ ] Draft to advisor on Dec 7; apply feedback.
- [ ] Chapter 6, proofreading, references, figures.
- [ ] Defense slides, 10-minute demo script, recorded backup video.
- [ ] Dec 15: submit.

## If you fall behind

Drop work in this order and write the dropped item into Chapter 6 as future work: AWS run, demo
gateway, SIGHUP reload, fault tests beyond killing one node. Never drop the quota accuracy test or
the benchmark report, because they carry the thesis claim.
