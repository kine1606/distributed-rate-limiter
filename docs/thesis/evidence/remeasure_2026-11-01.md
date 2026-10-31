# Re-measurement after the Week 1 to 9 edge-case tests (2026-11-01, commit df7985f)

Raw output: `results/2026-11-01_{matrix,open_loop,quota_accuracy,burst_accuracy,ab_customer_latency}/`.

## Verdict

| What | Result | Trust |
| --- | --- | --- |
| Tests | 345 of 345 ctest entries (344 C++, plus 50 Python tool tests in one entry) | correctness, independent of timing |
| Quota accuracy (test 1), 5 runs | 5 of 5 exact: Redis `used`, usage events and allowed request ids all 200 (Free) and 2,200 (Pay) | independent of timing |
| Burst bound (test 4), 5 runs | runs without a clock step: 34.7, 189.0, 381.5, as on 2026-10-22; 8 of 30 runs far over the model, 6 of them over N × C | **the Redis clock stepped (below)** |
| Matrix, open loop | most cells 5 to 25% faster than 2026-10-24; customer p99 2 to 4 times worse | **not comparable: the WSL clock ran 9% slow** |
| Customer latency, old (1542ef9) against new, alternating | same p50 (1.12 and 1.65 ms); p99 within noise (3.6 against 4.3 ms at 5,000/s, 10.2 against 13.8 at 10,000/s) | fair: both under the same clock |

## The clock fault

On this day the WSL2 virtual machine that runs both the limiter and Docker's Redis kept bad time.
Against Windows' clock, over 45 s:

| Windows | WSL wall clock (what Redis `TIME` reads) | WSL monotonic (what loadgen and the clients time with) |
| --- | --- | --- |
| 44.96 s | 46.95 s, in steps | 40.85 s (9% slow) |

The kernel uses the `tsc` clock source and Hyper-V's TimeSync service steps the wall clock to
Windows time: Redis `TIME` against the monotonic clock jumped +2.3 s and +3.7 s (and -0.9 s)
roughly every 30 s (a sampler of `TIME` every 20 ms).

What it does to each measurement:

- **The burst bound.** A forward step of X seconds refills every bucket by X × rate at once. The
  two runs over the bound that were examined each held one lump:
  steady, cap 1%: 432 admitted in about 1 s instead of 100 (332 extra = 3.3 s of refill at
  100/s); idle then burst, cap 5%: 164 extra (1.6 s). Runs without a step matched the model to
  0.4. The proof (3.5.3) takes all refill from Redis's clock, so a stepped clock is outside it;
  production Redis hosts usually slew NTP corrections rather than step them.
- **Throughput.** loadgen divides by monotonic seconds, 9% short of real ones, so today's
  checks/s read about 10% high. The matrix is kept as raw data, not as a new result.
- **Latency.** Tails are worse for both builds alike (the A/B), so the 2026-10-24 numbers stay
  the ones the thesis quotes.

`token_bucket.lua` stores the earlier time after a backward step; that keeps the total refill
equal to real elapsed time, so it stays as it is.

## Also seen

- **A hot Pay customer through one instance** (matrix, customer, hot key): 33,107 and 13,834
  calls answered `unverified` in runs 1 and 2 (Pay fails open), counted by loadgen since
  9220ba3. On 2026-10-24, the same cell's run 2 admitted 2,506 a second against a burst of 20,
  before loadgen counted such answers. This looks like the "about 30,000 uncounted" weak point
  in `defense_numbers.md`, with a repeatable trigger; its cause on a correct clock is not yet
  established.
- **Redis time per script call** rose from 31.0 to 31.7 µs (5,000/s) and 30.9 to 33.4 µs
  (10,000/s) between 1542ef9 and df7985f: `quota.lua` gained periods, boosts, top-ups, alerts
  and billing totals since. Small, and not visible in the A/B latency.

## To redo on a correct clock

`wsl --shutdown` from Windows, then check that `redis-cli -p 7001 TIME` and Windows agree
over a minute before `bench/run_matrix.sh`, `bench/open_loop.sh` and
`tests/eval/burst_accuracy.sh`.
