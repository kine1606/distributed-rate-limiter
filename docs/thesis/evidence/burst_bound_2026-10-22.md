# Evidence: the lease accuracy bound, measured and modelled (evaluation tests 4 and 12, 2026-10-22)

Goal G7: the over-admission of a leased limit is at most N × C in any window (N instances, chunk cap
C; Chapter 3, 3.5.3), and it is measured. This is the Week 6 gate: "measured over-admission at or
below the derived bound". Test 4 measures it on the real cluster at three chunk caps; test 12
compares the measurement with an independent model of the protocol.

## Setup

- **Scripts:** `tests/eval/burst_accuracy.sh` (driver, `RUNS=5`), `tests/eval/burst_client.py` (load,
  records every call), `tests/eval/bound_model.py` (the model), `tests/eval/plot_bound.py` (figure).
  Server at commit `2505906` plus the `lease_chunk_pct` rule field, committed as `87f9d9c`.
- **Limiters:** 4 instances, engine `leased`, defaults.
- **Rule:** `bound_test`, a leased token bucket of 1,000 tokens refilled at 100 a second, with
  `lease_chunk_pct` 1, 5 and 10, so C = 10, 50 and 100 and the worst case N × C = 40, 200 and 400.
- **Load:** one hot key through all four instances, open loop at 500 calls a second per instance
  (2,000 in all, 20 times the refill rate). The send lag stayed at or below 1.43 ms (p99); there were no errors.
  - *idle_burst*, the worst case the proof allows: one check per instance fills its lease
    (C − 1 tokens left), 12 s of silence let the bucket refill to 1,000, then 3 s of load.
  - *steady*: 10 s of load from a full bucket and empty leases.
- **Redis:** the development cluster, flushed before every run. Laptop (i5-12450H, WSL2), 5 runs per cell.

**What is measured.** For the allowed answers, timed when the client received them, the largest
excess over any exact bucket in any window: the maximum over a ≤ b of
admitted(a, b) − 1,000 − 100 × (b − a). An exact token bucket can never show more than 0. Redis
refills by whole milliseconds, so up to 0.1 token of noise is expected.

**The model.** `bound_model.py` is a simulation written from the protocol of 3.5.2, independently of
the C++ engine. It covers a Redis bucket that reproduces `token_bucket.lua` in milli-tokens and
whole milliseconds, and leases with top-up refills, early refill at 30%, partial grants, local denial
until the retry time, and the adaptive chunk. Each run's actual send times are replayed through it,
with a Redis round trip of 1 ms, and the same window measure is applied to its admissions. A
self-check (`--self-check`) confirms that the model never breaks the theorem on 200 random demands,
and that an exact bucket never shows more than one millisecond of refill.

## Result

![Over-admission against the chunk cap](../figures/burst_bound.png)

*Figure: over-admission in tokens against the chunk cap, measured (median of 5 runs; the range is
smaller than the marker) and predicted by the model, with the worst case N × C.*

Over-admission in tokens, medians of 5 runs (range):

| Chunk cap | Worst case N × C | Idle, then burst: measured | model | Steady: measured | model |
| --- | --- | --- | --- | --- | --- |
| 1% (C = 10) | 40 | **34.7** (34.6 to 34.8) | 34.4 | −0.1 (−0.2 to 0.0) | −0.2 |
| 5% (C = 50) | 200 | **189.0** (188.9 to 189.1) | 188.8 | −1.1 (−1.1 to −1.1) | −1.3 |
| 10% (C = 100) | 400 | **381.4** (381.4 to 381.5) | 381.4 | −1.5 (−8.0 to −1.3) | −1.5 |

- **The gate is met.** The largest over-admission measured in 30 runs was 381.5 tokens against a bound
  of 400, and every run was below its bound.
- **The bound is nearly reached when it should be.** After an idle period each instance holds C − 1
  tokens and the bucket is full again, so a burst can admit about N × (C − 1) more than any exact
  bucket: 36, 196 and 396. The measured values are slightly lower, 34.7, 189.0 and 381.4, because part
  of the bucket's refill lands while the window is still open. The bound is tight; it is not merely
  safe.
- **Under steady overload, leasing costs no accuracy.** Over-admission was at most 0.0, and over a
  whole run the leases admitted slightly less than an exact bucket would have for the same calls.
  The tokens still held in leases at the end of a run go unused.
- **The model predicts the measurement.** For the burst, measured and predicted values differ by 0.0
  to 0.3 tokens. A round trip of 0.5 ms or 2 ms instead of 1 ms changes the prediction by at most 0.6.
  The admitted counts are identical in 29 of 30 runs (table below). The exception (5%, run 1) admitted
  8 fewer than the model, which is stricter.

Admitted calls per run (runs 1 to 5), against the model and against one exact bucket fed the same
calls:

| Scenario, cap | Measured over-admission | Model | Admitted | Model admitted | Exact bucket admits |
| --- | --- | --- | --- | --- | --- |
| idle_burst, 1% | 34.6 / 34.7 / 34.7 / 34.6 / 34.8 | 34.5 / 34.4 / 34.5 / 34.4 / 34.4 | 1,337 in every run | 1,337 in every run | 1,304 |
| idle_burst, 5% | 189.0 / 189.1 / 189.1 / 188.9 / 188.9 | 188.8 in every run | 1,477 / 1,485 / 1,485 / 1,485 / 1,485 | 1,485 in every run | 1,304 |
| idle_burst, 10% | 381.4 / 381.5 / 381.4 / 381.4 / 381.4 | 381.2 / 381.4 / 381.4 / 381.4 / 381.4 | 1,676 in every run | 1,676 in every run | 1,304 |
| steady, 1% | 0.0 / −0.1 / −0.1 / −0.2 / 0.0 | −0.2 / −0.2 / −0.2 / −0.3 / −0.3 | 1,999 / 1,995 / 1,999 / 1,999 / 1,996 | 1,999 in every run | 2,000 |
| steady, 5% | −1.1 in every run | −1.3 / −1.3 / −1.1 / −1.3 / −0.9 | 1,980 / 1,982 / 1,980 / 1,980 / 1,986 | 1,980 / 1,980 / 1,980 / 1,980 / 1,979 | 2,000 to 2,001 |
| steady, 10% | −8.0 / −1.5 / −1.5 / −1.3 / −1.4 | −1.5 in every run | 1,956 in every run | 1,956 in every run | 2,000 to 2,002 |

The steady 10% run at −8.0 admitted the same 1,956 calls as the other four. The difference therefore
lies in the client's timestamps, not in what the limiter admitted.

## What it means for the design

The cap is the operator's knob, and the trade-off is now measured rather than derived:

| Cap | Worst over-admission measured (4 instances, limit 1,000) | Cost per check |
| --- | --- | --- |
| 1% | 35 tokens | about one Redis call per 10 checks under load |
| 5% | 189 tokens | about one per 50 |
| 10% | 381 tokens | about one per 100 |

The worst case needs a quiet period long enough for the bucket to refill while the leases stay full,
followed by a burst on every instance at once. Under sustained load the leased limit is as strict as
an exact one.

## Limits

- One hot key, four instances on one laptop. The bound holds per key, so more keys do not change it;
  more instances raise it linearly (N × C), which this run did not vary.
- Over-admission is timed when the client receives each answer, not when the limiter decides it.
  The model and the measurement agree to within 0.3 tokens, so the effect is small.
- Raw output: `results/2026-10-22_burst_accuracy/` (git-ignored).
