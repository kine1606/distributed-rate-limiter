# Evidence: Redis timeouts, failure policy per plan and graceful shutdown (2026-10-21, Week 5)

Three measurements behind the Week 5 reliability work: how the Redis timeout and the number of Redis
clients were chosen, what each plan gets while a Redis master is down, and what a graceful shutdown
gives back. One laptop, the development cluster (3 masters, no replicas, no persistence), the limiter
at commits `9caaed4` and `04cff8b` plus the uncommitted shutdown change.

## A. Choosing the Redis timeout and the number of Redis clients

The plan asked for a 5 to 10 ms socket timeout. One server, engine `strict`, rule `daily_quota`,
10,000 Free customers re-seeded before each run, `bench/loadgen` with 12 connections x 16 calls in
flight, 8 s measured, one run each. "Timeouts" and "breaker opens" are lines in the server log.

| Redis clients | Timeout | In flight | Checks/s | p99 | Timeouts | Breaker opens |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 10 ms | 192 | (most failed fast) | 34.2 ms | 14 | 14 |
| 1 | 200 ms | 192 | 8,896 | 50.5 ms | 0 | 0 |
| 2 | 200 ms | 192 | 23,960 | 20.3 ms | 0 | 0 |
| 4 | 200 ms | 192 | **36,283** | 13.2 ms | 0 | 0 |
| 8 | 200 ms | 192 | 29,555 | 17.1 ms | 0 | 0 |
| 4 | 10 ms | 192 | 35,642 | 13.8 ms | 8 | 5 |
| 4 | 20 ms | 192 | 33,968 | 15.0 ms | 1 | 2 |
| 4 | **50 ms** | 192 | 30,376 | 24.2 ms | **0** | **0** |
| 4 | 20 ms | 768 | 42,876 | 48.4 ms | 9 | 12 |

Findings:

- **A 10 ms timeout raised false alarms while Redis was healthy.** With one Redis client, replies
  waited tens of milliseconds behind its single I/O thread, which also finishes every gRPC call it
  answers. When a call times out, hiredis drops the whole connection and fails every call pending on
  it, so a single timeout was enough to open the circuit breaker: 14 openings in 8 seconds.
- **Four Redis clients, keys spread over them, give 4.1 times the exact-path throughput** (8,896 to
  36,283 checks/s) with p99 down from 50.5 to 13.2 ms. This is the weak spot found in the comparison
  with Envoy's ratelimit service (38,167 checks/s with one Redis call each,
  `ceiling_and_envoy_2026-10-19.md`), now mostly closed. Eight clients do worse than four on 12
  hardware threads.
- **50 ms is the default**, the smallest timeout tried with no false timeout at 192 calls in flight.
  Reaching the planned 5 to 10 ms needs the I/O thread freed of finishing gRPC calls (future work).
  `RL_REDIS_TIMEOUT_MS` and `RL_REDIS_LOOPS` change both.

## B. Failure policy per plan while a master is down

One server (engine `leased`, plan cache TTL 1 s), a Pay customer (`cust:pay:3`, fails open) and a
Free customer (`cust:3`, fails closed) whose keys live on the master at port 7002. Both called the
`customer` rule in turn, about 10 times a second each; the master's container was stopped for 3.3 s,
inside the cluster's 5 s node timeout, then started again.

| Phase | Pay customer | Free customer |
| --- | --- | --- |
| Before | 29 allowed `OK` | 4 allowed `OK`, 25 denied `RATE_LIMITED` (burst of 2) |
| Master down (3.3 s) | **59 of 59 allowed `UNVERIFIED`** | **59 of 59 denied `UNAVAILABLE`** |
| After restart | allowed or `RATE_LIMITED` | allowed or `RATE_LIMITED` |

- Each plan got its documented behaviour for the whole outage, longer than the 1 s plan TTL: the plan
  cache answered with the last plan it had read (stale if error).
- The circuit breaker opened 0.27 s after the first failed call and closed when the master answered again; with
  every check of this test on the dead master, consecutive failures reached the threshold.
- After the restart the master's data was gone (no persistence in the development cluster), so the
  Pay customer's plan hash was missing and the customer was treated as Free (the burst of 2 shows in
  the `RATE_LIMITED` answers), which is the documented rule for a missing plan. With replicas the
  promoted replica would keep the plan; that is the Week 5 gate's fault test.

## C. Graceful shutdown returns leased tokens

One server, engine `leased`, rule `api_default` (bucket of 1,000), 1,000 customers loaded for 4 s,
then SIGTERM. The server log after shutdown:

    shutting down, draining 0 in-flight checks
    returned 72194 unused tokens of 1000 leases to Redis (0 returns failed)

Customer `cust:7`'s bucket in Redis held 929 tokens before shutdown (the rest were leased to this
instance) and 1,000, its capacity, after: a restart no longer burns the leased part of every bucket.
Quota leases keep their units (at most one chunk, 0.1% of a large plan's day, per instance).

## D. Backpressure costs nothing measurable

The in-flight bound (`RL_MAX_IN_FLIGHT`, default 10,000; beyond it `RESOURCE_EXHAUSTED`) adds two
atomic operations per call. Same session, leased rule, 192 in flight: 76,712, 77,724 and 75,419
checks/s, against 79,538 for gRPC alone (`allow_all`): 96 to 98%, as before the bound (95% on
2026-10-19).

Limits: one laptop; the development cluster has no replicas, so a stopped master is an outage, not a
failover; single runs per cell in part A (enough to see false timeouts, not to rank close values).
