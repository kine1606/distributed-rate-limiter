# Evidence: single-instance throughput after Week 4 (2026-10-19)

What this shows: how many checks per second one limiter instance serves on the development laptop
for each kind of check, all measured in one session with the same load generator and settings.
Cite it for the lease-versus-strict comparison and for what a full customer check costs today.
It is not a cluster measurement: the 1M checks/s target is for many instances, and nothing here
measures more than one.

## Result

Median of 3 runs per row (range in brackets). One server instance, closed loop, 192 calls in flight.

| What is checked | Engine, rule | Checks/s, median (range) | p50 | p99 | Redis calls per check |
| --- | --- | --- | --- | --- | --- |
| gRPC alone, no limiter (the ceiling) | `allow_all`, `api_default` | 82,565 (80,280 to 83,971) | 2.33 ms | 5.79 ms | 0 |
| A rate-limit rule served from the lease | `leased`, `api_default` | **78,074** (72,369 to 78,381) | 2.44 ms | 6.60 ms | 0.016 (lease refills) |
| Exact daily quota | `strict`, `daily_quota` | 11,602 (11,580 to 11,659) | 15.02 ms | 35.73 ms | 1 script + plan reads |
| Full customer check, Free plans | `leased`, `customer` | 10,435 (9,803 to 10,490) | 17.87 ms | 34.22 ms | 2 scripts + plan reads |
| Full customer check, enterprise plans (leased quota) | `leased`, `customer` | **11,224** (11,148 to 11,226) | 15.69 ms | 37.32 ms | 1 script + 0.036 refills + plan reads |

Redis calls per check: lease refills are counted by the server (its shutdown line, over warm-up and
measurement): about 30,000 per leased `api_default` run of about 1.9 million checks, and exactly
10,000 per enterprise run, one quota refill per customer. Plan reads were not counted. With a 5 s
plan cache and 10,000 customers who all call within every 5 s, they are estimated at 2,000 a second,
about 0.2 per check in the last three rows. A check denied by burst skips the quota script.

Readings:

- A leased rule runs at 95% of gRPC alone (78,074 against 82,565 checks/s): the limiter adds about
  5% to the cost of a call, and is 6.7 times the exact check on the same load. 99.5% of its checks
  were served from the lease (about 10,200 of about 1.9 million waited for a refill).
- Every row that asks Redis once per check lands near 11,000 checks/s at this concurrency: the Redis
  round trip, not the limiter's code, sets the rate.
- Leasing an enterprise customer's quota removed one of the two Redis calls of a customer check
  (11,224 against 10,435 checks/s for Free customers, who need both), but the burst limit still
  costs one Redis call per check, so the full customer check stays near the strict rate. Leasing
  burst per customer is the change that would bring it toward the 78,074 of a leased rule; it is
  not built yet.
- In the Free-plan row about 90% of checks were allowed (187,316 of 208,708 in run 1). The rest were
  denied by the burst limit: each Free customer called about once a second against 2 tokens refilled
  at 2 a second, and random arrivals bunch up. The quota of 200 a day was not reached (about 26
  checks per customer per run). Every other row allowed all checks. No run had an error.

## Every run

| Cell | Run | Checks/s | Checks | Allowed | Errors | p50 ms | p99 ms | p99.9 ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| allow_all, api_default | 1 | 83,971 | 1,679,422 | 1,679,422 | 0 | 2.23 | 6.05 | 10.62 |
| leased, api_default | 1 | 78,381 | 1,567,618 | 1,567,618 | 0 | 2.44 | 5.31 | 9.33 |
| strict, daily_quota | 1 | 11,580 | 231,608 | 231,608 | 0 | 15.02 | 35.73 | 40.69 |
| leased, customer, Free | 1 | 10,435 | 208,708 | 187,316 | 0 | 17.87 | 34.22 | 42.49 |
| leased, customer, enterprise | 1 | 11,224 | 224,478 | 224,478 | 0 | 15.69 | 37.32 | 44.38 |
| allow_all, api_default | 2 | 82,565 | 1,651,298 | 1,651,298 | 0 | 2.33 | 5.31 | 8.93 |
| leased, api_default | 2 | 72,369 | 1,447,390 | 1,447,390 | 0 | 2.54 | 6.60 | 11.09 |
| strict, daily_quota | 2 | 11,602 | 232,031 | 232,031 | 0 | 15.69 | 35.73 | 40.69 |
| leased, customer, Free | 2 | 10,490 | 209,810 | 187,667 | 0 | 17.87 | 34.22 | 40.69 |
| leased, customer, enterprise | 2 | 11,148 | 222,963 | 222,963 | 0 | 15.69 | 37.32 | 44.38 |
| allow_all, api_default | 3 | 80,280 | 1,605,612 | 1,605,612 | 0 | 2.33 | 5.79 | 9.33 |
| leased, api_default | 3 | 78,074 | 1,561,481 | 1,561,481 | 0 | 2.44 | 6.60 | 11.09 |
| strict, daily_quota | 3 | 11,659 | 233,188 | 233,188 | 0 | 15.02 | 35.73 | 40.69 |
| leased, customer, Free | 3 | 9,803 | 196,069 | 178,069 | 0 | 19.48 | 37.32 | 52.77 |
| leased, customer, enterprise | 3 | 11,226 | 224,520 | 224,520 | 0 | 15.69 | 37.32 | 44.38 |

Latencies come from log-scale buckets of about 4% width, which is why several runs share a value.

## Setup

- Machine: Intel Core i5-12450H (12 threads), 7 GB RAM given to WSL2 (kernel
  6.6.114.1-microsoft-standard-WSL2), Ubuntu 24.04. Load generator, server and Redis all on this
  one laptop. Power mode and other applications were not controlled.
- Redis: the development cluster (`docker/docker-compose.dev.yml`): 3 masters, no replicas, no
  persistence, Redis 7.4.11 in Docker 28.5.1, flushed and re-seeded before every run.
- Server: `build/release/rate_limiter_server` (gcc 13.3, -O3 -march=native) at commit `6b0b048`,
  `config/rules.yaml`, defaults (plan cache 5 s, no batcher, no usage file).
- Load: `bench/loadgen` (C++, closed loop): 12 channels × 16 calls in flight, keys `cust:1` to
  `cust:10000` drawn uniformly, cost 1, 5 s warm-up discarded, 20 s measured, 1 s deadline per call.
  The same settings as the loadgen cells of the Week 3 gate.
- Customers: `tools/seed_plans.sh 10000` makes `cust:1..10000` Free (200 a day, burst 2). For the
  enterprise row the same keys were overwritten with the enterprise preset of `tools/plans.sh`
  (1,000,000 a day, burst 2,000 at 2,000/s, cap overage), so their quota is leased in chunks of 1,000.
- Order: the five cells ran in turn, three times over, so drift during the session hits every cell.

Limits of this evidence: closed-loop load hides stalls from the percentiles (coordinated omission;
the open-loop generator is Week 5), the Docker network's round trip is far shorter than a real
network's, and client and server compete for the same cores. Numbers from other sessions differ (the
Week 3 gate measured 71,364 checks/s for gRPC alone on the same settings), so compare rows within
this table only.

## How to reproduce

Raw output (one JSON line per run, and each run's server log) was written to
`results/2026-10-19_rps_now/`, which is git-ignored. The script, as run (only the `cd` line made
portable):

```bash
#!/usr/bin/env bash
# Current throughput, same settings as the Week 3 gate's loadgen cells (12 channels x 16 in flight,
# 5 s warm-up, 20 s measured), dev cluster flushed and seeded before every run.
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
port=7001 out=results/$(date +%F)_rps_now
mkdir -p "$out"
sum="$out/summary.txt"
echo "# rps_now at commit $(git rev-parse --short HEAD); loadgen 12 x 16, 20 s after 5 s warm-up" >"$sum"
source tools/plans.sh
seed() {  # $1 = free | enterprise for cust:1..10000
  for m in $(redis-cli -p $port cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do redis-cli -p "$m" FLUSHALL >/dev/null; done
  tools/seed_plans.sh 10000 >/dev/null
  if [ "$1" = enterprise ]; then
    cmds=$(mktemp)
    seq 10000 | awk -v p="${PLAN[enterprise]}" '{print "HSET {cust:" $1 "}:plan plan enterprise " p}' >"$cmds"
    for m in $(redis-cli -p $port cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do redis-cli -p "$m" --pipe <"$cmds" >/dev/null 2>&1 || true; done
    rm -f "$cmds"
    [ "$(redis-cli -c -p $port hget '{cust:10000}:plan' daily)" = 1000000 ]
  fi
}
for r in 1 2 3; do
  for cell in "allow_all api_default free" "leased api_default free" "strict daily_quota free" "leased customer free" "leased customer enterprise"; do
    set -- $cell
    seed "$3"
    build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml 127.0.0.1:$port lua "$1" >"$out/$1_$2_$3_r$r.server.log" 2>&1 &
    sp=$!
    sleep 1
    echo "$1_$2_$3_r$r: $(build/release/bench/loadgen 127.0.0.1:50051 "$2" 20 12 16 10000 5)" | tee -a "$sum"
    kill $sp; wait $sp 2>/dev/null || true
  done
done
```

Only run it against the throwaway development cluster: it runs `FLUSHALL` on every master.
