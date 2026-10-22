# Evidence: one noisy tenant and everyone else (evaluation test 11, 2026-10-23)

What one flooding tenant does to the other tenants of the same limiter and Redis.
Script: `tests/eval/noisy_neighbour.sh` (`RUNS=3`), one limiter (engine `leased`, defaults), the
development cluster (3 masters). Laptop: load generator, limiter and Redis share 12 hardware threads.

## Setup

- **The others:** Pay customers `cust:pay:1..1000` on the `customer` rule, open loop at 5,000 checks
  a second, measured for 10 s after 1 s of warm-up.
- **The noisy tenant:** one key at 20,000 calls a second, open loop, starting 1 s before the others.
  - *none*: no noisy tenant, the baseline.
  - *over_quota*: the tenant has used up its daily quota, so the deny cache answers every call.
  - *over_burst*: the tenant is within its daily quota but far over its burst limit of 20 a second.
    Every call runs the burst script in Redis, which denies it.
- **Redis load:** the calls a second answered by the master that holds the noisy tenant, from
  `INFO commandstats` (`EVALSHA`, `EVAL`, `HGETALL`), over the whole phase. A third of the others'
  customers live on that master too.

## Result

Medians of 3 runs (range in brackets). The others got every answer right in every run: 50,000 `OK`
each time, no error, none skipped.

| Noisy tenant | Others' p50 | Others' p99 | Others' p999 | Redis calls/s on the noisy tenant's master | Noisy tenant's answers |
| --- | --- | --- | --- | --- | --- |
| none | 1.02 ms | 4.47 ms (3.92 to 4.66) | 24.2 ms | 3,319 | |
| over quota | 1.80 ms | 14.4 ms (7.8 to 57.5) | 42.5 ms | **3,319** | 260,000 `QUOTA_EXHAUSTED` |
| over burst | 2.44 ms | **31.4 ms** (30.0 to 32.8) | 52.8 ms | **25,233** | 259,720 `RATE_LIMITED`, 280 `OK` |

- **An over-quota flood costs Redis nothing.** The deny cache answered all 260,000 of the flood's
  calls on the limiter, and the master's call rate did not change (3,319 a second, with and without
  the flood). The others' p99 still rose from 4.5 to 14.4 ms, because the flood's 20,000 gRPC calls a
  second share the CPU with them. On this laptop that includes the load generator itself, so part of
  the rise is the test bench.
- **An over-burst flood is the weak spot.** The deny cache holds quota denials only. A tenant within
  its quota but over its burst limit costs one Redis script per call: the master's load went from
  3,319 to 25,233 calls a second, and the others' p99 rose sevenfold, to 31.4 ms. Their answers stayed
  correct.
- **The fix is already in the design, for leased rules.** When Redis says a leased bucket is empty, the
  lease denies locally until the retry time Redis returned (3.5.2, step 7). Applying the same local
  denial to the customer's burst check, or leasing the burst per customer (the future work in 3.2.1),
  would turn the over-burst flood into the over-quota case. It was not built during the feature freeze.
  The local denial would have to be kept per cost, because the retry time depends on the cost denied,
  and dropped when the plan's burst changes.

## Limits

- One instance on one laptop, so CPU is shared with the load generator. Over separate machines, the
  over-quota flood would cost the others less.
- One noisy tenant at one rate; the others all on the customer rule.
- Raw output: `results/2026-10-23_noisy_neighbour/` (git-ignored).
