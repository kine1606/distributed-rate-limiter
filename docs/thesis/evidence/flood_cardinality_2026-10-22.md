# Evidence: flood and cardinality (evaluation tests 5 and 6, 2026-10-22)

Goal G8, an over-quota flood refused locally, and the cost of many customers, as `docs/EVALUATION.md`
tests 5 and 6 ask. One limiter instance (engine `leased`, defaults, metrics on), the development
cluster (3 masters, Redis 7.4.11), 3 runs, the medians reported with their range.
Script: `tests/eval/flood_cardinality.sh` at commit `8e4f22e`. Laptop (i5-12450H, WSL2), with the
load generator, the limiter and Redis on the same machine.

## A. Flood: one customer out of quota, retrying flat out

**Setup.** A Free customer (`cust:flood:1`, quota 200, burst limit raised) first uses up the quota.
One more call puts the customer in the deny cache. Then the customer calls the `customer` rule as
fast as the load generator can: 12 connections with 16 calls in flight each, for 10 s. After that it
sends at a fixed 10,000 calls a second for 10 s, to measure the latency of the denials. The Redis
calls for this customer are read from `INFO commandstats` (`EVALSHA`, `EVAL`, `HGETALL`) on the master
that owns the customer's slot, before and after each phase. No other customer was active.

| Phase | Denials per second | Redis calls in 10 s | Deny-cache hits | p50 | p99 | p999 |
| --- | --- | --- | --- | --- | --- | --- |
| Flat out | **74,881** (73,960 to 78,906) | **2** in every run | every check | 2.54 ms | 5.55 ms | 9.33 ms |
| Fixed 10,000/s | 10,000 | **2** in every run | 100,000 of 100,000 | **0.24 ms** | **0.61 ms** (0.51 to 2.90) | 3.92 ms |

- **A flood costs Redis nothing.** About 750,000 denials in 10 s cost two Redis calls, both of them
  re-reads of the plan when its five-second cache entry expired. Without the deny cache, each denial
  would have cost at least the burst script: about 75,000 Redis calls a second for one customer.
- **A denial costs what a lease hit costs.** At full speed the instance denied 74,881 calls a second,
  its gRPC rate for any check. The shard mutex that a denied customer's lookups share (3.6.1) did not
  limit it, so the shortcut of locking that shard stays.
- **Every answer was `QUOTA_EXHAUSTED`**, with the reset time; no call failed.

## B. Cardinality: 1,000,000 customers

**Setup.** `tools/seed_plans.sh 1000000` writes 1,000,000 Free plans, 1,000,000 Pay plans and 10
enterprise plans: 2,000,010 plan hashes. Redis memory is summed over the masters (`INFO memory`)
before and after seeding. Then, on a fresh limiter each time, two rules are run for 20 s after 2 s of
warm-up (12 connections × 16 in flight), with keys drawn uniformly from 10,000 or from 1,000,000 Free
customers:
- the `customer` rule: burst, then the daily quota, both exact;
- `api_default`: a token bucket served from leases.

The size of each kind of key is the median `MEMORY USAGE` of up to 90 keys, sampled right after each
load. Key counts are no use here: an idle burst bucket expires within seconds.

**Redis memory per key**

| Key | Bytes | How measured |
| --- | --- | --- |
| Plan hash `{cust:N}:plan` | **171** (160 by `MEMORY USAGE`) | `INFO` delta over 2,000,010 plans: 343.5 MB |
| Burst bucket `{cust:N}:burst:tb` | 112 | `MEMORY USAGE` |
| Daily counter `{cust:N}:daily_quota:q` | 112 | `MEMORY USAGE` |
| Leased rule bucket `{cust:N}:api_default:tb` | 120 | `MEMORY USAGE` |
| Stored request-id answer | 181 | `INFO` delta over 100,000 (Chapter 3, 3.6.2) |

`MEMORY USAGE` leaves out about 11 bytes per key of table overhead (160 against 171 for the plans).
A Free customer active today therefore costs about 171 + 123 + 123 ≈ 420 bytes, and 1,000,000 such
customers about 420 MB across the cluster, or 140 MB per master. Buckets disappear within seconds of
going idle, so customers who called earlier in the day cost about 300 bytes.

**Throughput and limiter memory**

| Rule | Customers | Checks/s | p50 | p99 | Limiter memory | Redis work per check |
| --- | --- | --- | --- | --- | --- | --- |
| `customer` | 10,000 | 38,201 (36,995 to 39,268) | 4.87 ms | 11.59 ms | 52 MB | 2 scripts; 0.06 plan reads |
| `customer` | 1,000,000 | **28,365** (27,429 to 28,384) | 6.60 ms | 12.63 ms | 85 MB | 2 scripts; **0.89 plan reads** |
| `api_default` (leased) | 10,000 | 72,902 (70,506 to 75,762) | 2.66 ms | 5.79 ms | 49 MB | 0.017 refills |
| `api_default` (leased) | 1,000,000 | **47,320** (45,399 to 48,150) | 3.92 ms | 10.17 ms | **224 MB** | **0.62 refills** |

Plan reads and refills are the server's own counters (`rl_plan_cache_reads_total`,
`rl_lease_refills_total`), over warm-up and measurement. Checks include those warm-up seconds in the
per-check ratios.

- **1,000,000 customers cost a quarter of the customer rule's throughput (−26%), and the reason is
  the plan cache.** At 28,000 checks a second spread over 1,000,000 customers, a customer's next call
  comes long after its five-second cache entry expired. Nine checks in ten therefore read the plan
  first, a third Redis round trip on top of the two scripts. This is the plan-read cost that 3.8
  predicted from the formula; it is now measured.
- **Leases help only keys that are called again.** With 1,000,000 keys each key saw about one check
  in the run, so 62% of the checks waited for a refill and throughput fell by 35%, still about 1.7
  times the exact customer rule. Memory grew by about 175 MB for some 650,000 leases, about 280 bytes
  each including the allocator's share (a lease itself is 192 bytes). Leases are never evicted, so
  this memory grows with the distinct keys seen, the known shortcut in the lease table.
- No run had an error.

## Limits

- One instance, one laptop, uniform keys. Real traffic is skewed: most calls come from a few busy
  customers, which keeps their plans cached and their leases warm. Uniform keys over 1,000,000
  customers are the hard case for both caches.
- The flood is one customer on one instance. With N instances, each learns the denial with one Redis
  call of its own (3.6.1).
- Raw output: `results/2026-10-22_flood_cardinality/` (git-ignored).
