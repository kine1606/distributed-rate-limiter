# Evidence: quota accuracy across 4 instances with retries (evaluation test 1, 2026-10-22)

Goals G1 (no over-admission of a quota) and G2 (a retried call counted once), checked together as
`docs/EVALUATION.md` test 1 asks: four limiter instances, two customers hammered beyond their quota
through all of them, a tenth of the requests sent twice, 20 runs.

## Setup

- **Script:** `tests/eval/quota_accuracy.sh` (driver) and `tests/eval/quota_client.py` (client),
  `RUNS=20`, at commit `2505906` plus the scripts.
- **Limiters:** 4 instances of `build/release/rate_limiter_server`, engine `leased` with defaults,
  each writing usage events to its own file (`RL_USAGE_FILE`).
- **Redis:** the development cluster (3 masters, Redis 7.4.11 in Docker), flushed before every run.
- **Customers:** `cust:eval:free`, the Free plan (200 a day, `hard`), and `cust:eval:pay`, the Pay
  plan (2,000 a day, `grace` of 200, so 2,200 calls may pass). Both plans have their burst limit raised
  to 100,000, so the daily quota, not the burst, is the limit that binds.
- **Load:** twice each allowance, 400 and 4,400 calls of the `customer` rule, cost 1, each with a
  fresh request id, in random order, 64 at a time, each to a random instance. A tenth of the requests
  are sent twice at the same moment, to two different instances, with the same request id. That is
  the hardest case for deduplication: both copies reach Redis together, so neither instance can know
  about the other.
- **Machine:** Intel Core i5-12450H (12 threads), WSL2; client, limiters and Redis on one laptop.

## What is checked

For each customer and run, three counts that come from different places must all equal the
allowance:

1. **The client's count:** request ids that got at least one allowed answer.
2. **Redis:** the `used` field of the customer's daily counter after the run.
3. **Usage events:** the `allowed` units of the four instances' usage files, summed by
   `tools/usage_report.py` after a graceful stop.

If (2) or (3) is larger than (1), a call was counted twice. If (1) is larger than the allowance, the
quota was over-admitted.

## Result

Every run was exact. Columns: client ids allowed / Redis `used` / usage events; then the requests
that were sent twice (Free + Pay).

| Run | Free (allowance 200) | Pay (allowance 2,200) | Sent twice | Counted twice |
| --- | --- | --- | --- | --- |
| 1 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 29 + 438 | 0 |
| 2 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 41 + 431 | 0 |
| 3 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 29 + 428 | 0 |
| 4 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 30 + 448 | 0 |
| 5 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 41 + 424 | 0 |
| 6 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 33 + 443 | 0 |
| 7 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 37 + 437 | 0 |
| 8 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 42 + 443 | 0 |
| 9 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 26 + 441 | 0 |
| 10 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 46 + 469 | 0 |
| 11 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 50 + 462 | 0 |
| 12 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 42 + 445 | 0 |
| 13 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 43 + 423 | 0 |
| 14 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 32 + 427 | 0 |
| 15 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 42 + 480 | 0 |
| 16 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 54 + 439 | 0 |
| 17 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 41 + 437 | 0 |
| 18 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 39 + 442 | 0 |
| 19 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 46 + 435 | 0 |
| 20 | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 36 + 437 | 0 |

- **No over-admission and no double count in 20 of 20 runs**: 96,000 requests, 9,608 of them sent
  twice at once (779 Free, 8,829 Pay).
- **Every request got one answer**: no request id got an allowed answer from one instance and a denial
  from the other (`mixed_ids` 0 in every run), and no call failed.
- **Retries got the first answer.** The Free customer's `OK` answers numbered 212 to 229 per run for
  200 allowed ids: the extra ones are the second copies of allowed requests, answered from the
  stored result and not counted again.
- Once the quota was used up, the rest were denied `QUOTA_EXHAUSTED`, by Redis or from the deny cache.
  No other reason appeared.

## On the way

The first attempt at 20 runs stopped after one, because the script's test ports (50071 to 50074)
lay in Linux's ephemeral port range: a Redis connection of one instance had already been given port
50072 as its local end, so the next instance could not listen on it. The script now uses ports below
32768. That run's usage files also held the events of an earlier trial, which the usage log appends
to; the script now clears them before each run. Neither was a fault of the limiter.

## Limits

- One laptop, so "concurrent" means 64 calls in flight over loopback, not separate machines.
- The retries are sent at the same moment as the original. A retry that arrives after the quota ran
  out at an instance that already holds the denial is denied there (Chapter 3, 3.4.3): it is still
  not counted twice, but its answer differs from the original. This run did not produce that case.
- Raw output: `results/2026-10-22_quota_accuracy/` (git-ignored).
