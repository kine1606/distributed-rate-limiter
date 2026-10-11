# Thesis outline

Target 55 to 65 pages (check your faculty's required format and language first). Write 30 minutes
every working day about what you built that day; December is for review, not first drafts. Each
chapter lists its sections, what goes in each, and where the material comes from in this repo.

## Chapter 1. Introduction (4 to 6 pages, draft in Week 2)

1.1 **Context: selling APIs.** Companies that sell API access (AI models, SMS and OTP, maps, data
feeds) sell request quotas by plan. Every call must answer "has this customer used what they paid
for?" in real time.
1.2 **Problem.** Managed gateways enforce quotas only approximately (AWS usage plans are "not hard
limits, applied on a best-effort basis"). Wrong answers either give the product away or block
paying customers.
1.3 **Goals.** Exact per-customer daily quotas; about 1M checks/s across the cluster; p50 < 500 µs
and p99 < 2 ms server-side; documented behaviour when Redis fails.
1.4 **Contributions.** HINT: three or four sentences, each a thing you built and measured (the
quota design, the lease with a measured accuracy bound, the evaluation).
1.5 **Structure of the thesis.**

## Chapter 2. Background and related work (8 to 10 pages, Weeks 1 to 2)

2.1 Rate limiting algorithms: fixed window, sliding window, token bucket; why a daily quota plus a
token bucket fits the business model.
2.2 Redis: single-threaded command execution, Lua atomicity, `EVALSHA`, Redis Cluster slots and
hash tags, failover.
2.3 gRPC and Protocol Buffers: HTTP/2, the callback API, deadlines.
2.4 Related systems compared in one table: AWS API Gateway usage plans, Kong, Envoy ratelimit,
Unkey, Upstash Ratelimit. HINT: columns: exactness, latency, deployment, open source.
Source material: [lua/README.md](../../lua/README.md), [proto/README.md](../../proto/README.md).

## Chapter 3. Design (12 to 14 pages, Week 5)

3.1 Requirements (functional and non-functional).
3.2 Architecture (diagram from `DESIGN.md` section 2).
3.3 Data model.
3.4 Quota and overage logic.
3.5 Lease protocol and the accuracy bound (derivation).
3.6 Deny cache and idempotency.
3.7 Failure policies and failure modes.
3.8 Capacity math.
Source material: [DESIGN.md](../DESIGN.md), the Quota Service Flow page.

## Chapter 4. Implementation (8 to 10 pages, Week 7)

4.1 Project structure and build (CMake presets, sanitizers).
4.2 Lua scripts (annotated excerpts).
4.3 Redis client choice and the batcher (spike numbers).
4.4 Lease table: memory layout, atomics, single-flight (code excerpts with memory-order reasons).
4.5 gRPC server: callback API, `Finish()` discipline, backpressure, shutdown.
4.6 Reliability: circuit breaker, retries, health.
4.7 Testing approach.
Source material: module READMEs under `src/`.

## Chapter 5. Evaluation (12 to 14 pages, Week 8)

5.1 Setup (hardware, software versions, topology).
5.2 to 5.10 One section per test in [EVALUATION.md](../EVALUATION.md).
5.11 Profile and bottlenecks (flamegraph).
5.12 Extrapolation to 1M checks/s with assumptions.
5.13 Discussion: what the numbers mean, and threats to validity (laptop, Docker networking).

## Chapter 6. Conclusion (3 to 4 pages, Week 9)

6.1 Results against each goal from 1.3, in a table: goal, result, met or not.
6.2 Limitations.
6.3 Future work: multi-region, monthly quota pools, billing integration, Redis Streams for usage.

## Writing hints

- **Every number has a source.** A figure or table in Chapter 5 names its result file.
- **Every design choice has a reason and a rejected alternative.** "We use Redis `TIME` instead of
  the caller's clock because..." is the sentence examiners look for.
- **Write limitations the day you meet them,** in `DESIGN.md` section 11, then move them here.
