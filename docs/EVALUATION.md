# Evaluation

Status: test design from the plan; scripts and results are added in Weeks 6 to 8. Each test has a
script in `tests/eval/`, a raw result in `results/`, and a table or figure in Chapter 5.

Every run records hardware, instance count, Redis masters, chunk size, key distribution and the
load-generator command, is repeated at least 5 times, and reports the median.

The tests check the two halves of the thesis claim separately: exact per-customer quotas first
(tests 1 to 3), then throughput and robustness (tests 4 to 9).

## 1. Quota accuracy

- **Setup:** 4 limiter instances; one Free and one Pay customer hammered by many concurrent clients
  through all instances; 10% of calls retried with the same `request_id`.
- **Pass:** exactly 200 and 2,000 allowed (from `usage_report.py` and from Redis `used`); zero
  double counts.
- **Script:** `tests/eval/quota_accuracy.sh`
- **Hint:** run it 20 times, not 5. A race that shows up once in 20 runs is still a bug.

Result (2026-10-22, 20 runs, `docs/thesis/evidence/quota_accuracy_2026-10-22.md`): exact in every
run. The Pay plan's grace of 10% makes its allowance 2,200; the plans' burst limits were raised so
that the quota binds. Each cell: request ids allowed / Redis `used` / usage events.

| Runs | Free allowed (200) | Pay allowed (2,200) | Sent twice at once | Duplicates counted |
| --- | --- | --- | --- | --- |
| 1 to 20, every run | 200 / 200 / 200 | 2,200 / 2,200 / 2,200 | 9,608 in all (26 to 54 Free, 423 to 480 Pay per run) | 0 |

## 2. Plan changes and reset

- **Setup:** upgrade a Free customer mid-day with `rlctl`; downgrade a Pay customer; run across
  00:00 UTC (or fake it by editing the stored `day` field, as the Lua tests do).
- **Pass:** upgrade applies on the next request on every instance (record the delay); reset happens
  once, at the same moment everywhere.

## 3. Overage policies

- **Setup:** three customers on `hard`, `grace` and `cap`, each past 2,000.
- **Pass:** request 2,001 is blocked, allowed free, and allowed as overage; the cap is respected
  exactly; overage events are marked in the usage log.

## 4. Burst accuracy bound

- **Setup:** one hot enterprise key from all instances, lease mode, chunk caps of 1%, 5% and 10%.
- **Result:** admitted count against `limit + N × C`; plot over-admission against chunk size.
- **Hint:** this figure is the evidence for the trade-off in Chapter 3. Label both axes with units.

## 5. Flood

- **Setup:** one over-quota client retrying as fast as possible.
- **Result:** Redis calls per second for that customer (expect near zero thanks to the deny cache)
  and the latency of denials.

## 6. Cardinality

- **Setup:** load spread over 1 million customer keys (seeded with `seed_plans`).
- **Result:** throughput, latency, Redis memory per key (`INFO memory` before and after ÷ keys).

## 7. Strict versus lease

- **Setup:** identical load in both modes.
- **Result:** throughput and p50, p99, p999 for each.

| Mode | Throughput | p50 | p99 | p999 |
| --- | --- | --- | --- | --- |
| Strict | | | | |
| Leased | | | | |

## 8. Scaling

- **Setup:** 1, 2 and 4 limiter instances (bonus: AWS on separate machines).
- **Result:** throughput per instance and how close scaling is to linear.
- **Hint:** on one laptop, instances and the load generator share CPU cores, so scaling will look
  worse than it is. Pin them to separate cores (`taskset`) and say so.

## 9. Faults

- **Setup:** kill a Redis master; add 20 ms latency; drop connections.
- **Result:** behaviour per plan's failure policy (allowed, denied, unverified counts), breaker
  state over time, recovery time.

## 10. Ablation

- **Setup:** the Week 7 load with one mechanism switched off at a time: lease, batcher, early
  refill, adaptive chunk, deny cache, plan cache, cache-line alignment.
- **Result:** throughput and p99 change per mechanism, in one table.

## 11. Noisy neighbour

- **Setup:** one tenant sends ten times its quota while the others run normal load.
- **Result:** the other tenants' p99 with and without the flood.

## 12. Bound model check

- **Setup:** a Python simulation of N instances leasing chunks of size C predicts over-admission;
  compare with test 4 at chunk caps of 1%, 5% and 10%.
- **Pass:** measured over-admission at or below the prediction and the worst case N × C.

## 13. Scalability model

- **Setup:** throughput at 1, 2, 4 instances and 1 to 8 worker threads, fitted to the Universal
  Scalability Law.
- **Result:** contention and coordination coefficients; projected instances for 1M checks/s.
- **Hint:** report latency from the open-loop generator (fixed arrival rate, latency from the
  scheduled send time); a closed-loop tool hides stalls from p99.

## Extrapolation to 1M checks/s

Use the measured per-instance throughput (test 7) and the scaling result (test 8). Write every
assumption next to the number. It is never presented as a measurement.
