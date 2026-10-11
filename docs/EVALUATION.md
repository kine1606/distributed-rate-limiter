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

| Run | Free allowed | Pay allowed | Duplicates counted |
| --- | --- | --- | --- |
| | | | |

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

## Extrapolation to 1M checks/s

Use the measured per-instance throughput (test 7) and the scaling result (test 8). Write every
assumption next to the number. It is never presented as a measurement.
