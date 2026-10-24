# Chapter 6. Conclusion

This thesis set out to show that a multi-tenant rate limiter can keep quotas exact while answering
most checks without a round trip to shared storage. The design splits the two kinds of limit:
- **Daily quotas, which are billed, are decided in Redis by atomic Lua scripts:** exact, deduplicated
  by request id, and reset once by Redis's clock.
- **Burst limits are served from leases:** tokens an instance takes from Redis in chunks and hands out
  with one atomic operation. Their over-admission is proved to stay below N × C, the number of
  instances times the chunk cap.

Between the two sit:
- a deny cache, which answers a customer out of quota locally;
- a plan cache, kept fresh by Redis Pub/Sub;
- a failure policy per plan: paid plans fail open, Free plans fail closed.

## 6.1 Results against the goals

*Table 6.1: The goals of Section 1.3 and what the evaluation found.*

| Goal | Result | Status |
| --- | --- | --- |
| G1 quota never over-admitted | four instances, 20 of 20 adversarial runs exact; 22.6 million checks equal to a reference model; 10 of 10 planted bugs caught (5.2) | met |
| G2 retries not counted twice | 9,608 duplicates sent at once to two instances, each counted once (5.2) | met |
| G3 plan changes apply at once | an upgrade reached two instances 22 ms after the admin tool started; usage kept (5.3) | met |
| G4 one reset instant | a full counter from yesterday resets once though two instances meet it at once; 34,425 simulated rollovers (5.3) | met |
| G5 about 1,000,000 checks/s on a cluster | estimated, not measured: 15 to 50 instances for leased rules, depending on the latency target, and 17 Redis masters for customer checks, about $18,500 a month on demand (5.11) | estimated |
| G6 p50 < 500 µs, p99 < 2 ms | leased rules: p50 0.43 ms, p99 1.45 ms at 20,000 checks/s per instance. The customer check: p99 1.88 ms at 5,000/s, but p50 0.94 ms, because it needs two Redis round trips (5.6) | met for leased rules; p99 only for customer checks |
| G7 over-admission at most N × C | proved; 100,000 seeded schedules; measured 34.7, 189.0 and 381.4 tokens against 40, 200 and 400, with a model within 0.3 tokens (5.4) | met |
| G8 floods refused locally | 74,881 denials a second cost Redis 2 calls in 10 s; without the deny cache, throughput falls by 64.6% (5.5, 5.9) | met |
| G9 documented behaviour when Redis fails | a slow, silent or killed master costs only its own customers their plan's policy; no call failed; recovery in 4 to 5 s after a kill (5.7) | met |

**The engineering rules held,** measured rather than assumed:
- *The lease hot path* takes no lock and makes no allocation (microbenchmark, profile). The limiter's
  own code is 4 to 6% of the CPU under load.
- *Every test passes under the sanitizers:* 240 tests in release, AddressSanitizer and
  ThreadSanitizer, with 97.3% line coverage.

**One intermediate target was not met.** A leased instance at four times the strict baseline would be
101,204 checks/s. On the laptop, gRPC with no limiter at all runs at 82,700 to 91,200, so no limiter
can reach that on one instance with one check per call. With `CheckBatch` of 10 checks per call, one
instance reached 400,187 checks/s (5.10).

**What the numbers say about the design:**
- **Leases pay where keys repeat:** 1.7 times strict on Zipf keys, 5.2 times on one hot key, and
  nothing when keys are spread so thin that each is seen once (5.6).
- **The customer check's cost is two Redis round trips.** That cost, not the limiter's code, sets its
  latency and the number of Redis masters it needs.

## 6.2 Limitations

**The evidence:**
- **One laptop.** Load generator, limiter and Redis share 12 hardware threads, and Redis runs in
  Docker on loopback. Throughput is a lower bound, the noise is often ±10%, and scaling across
  instances is estimated, not measured. The Universal Scalability Law could not stand in for that
  measurement: it does not fit one instance's curve (5.6).
- **Synthetic traffic.** Uniform and Zipf keys model traffic; no trace of real traffic was replayed.

**The design:**
- **One region.**
- **A failover can lose increments** that a master acknowledged but had not replicated, so the
  promoted replica counts low. The usage events are the durable record for billing.
- **Paid plans fail open.** While Redis cannot answer, their calls are allowed uncounted.
- **Burst checks are not deduplicated,** and an over-burst flood within quota costs a Redis call per
  attempt, which slowed other tenants on the same master (5.8).
- **Plan reads grow with instances times customers,** and leases are never evicted.

## 6.3 Future work

- **Run the evaluation on separate machines** (planned as a cloud run and not done). This would turn the
  extrapolation of 5.11 into a measurement and test its assumptions, starting with linear scaling
  across instances and a master's capacity for the quota script.
- **Lease the burst limit per customer, or deny locally until Redis's retry time.** This closes the
  noisy-neighbour gap and moves most customer checks to the leased path.
- **Route each customer to one instance** (consistent hashing in the gateway). Plan reads would fall
  from N × A to A per cache lifetime, and the customer's lease bound from N × C to C.
- **Batch at the gateway.** `CheckBatch` raised one instance's throughput 5.5 times, since gRPC's
  cost per call dominates.
- **Multi-region limits:**
  - each region with its own cluster and a share of the global quota;
  - shares rebalanced asynchronously, at the price of a bound like N × C between regions.
- **Monthly quota pools,** shared across a customer's projects, beside the daily quota.
- **Billing integration:**
  - usage events written to Redis Streams or a log, rather than a local file;
  - a consumer that reconciles them with the counters.
- **Lease eviction** for long-running instances over many keys.
