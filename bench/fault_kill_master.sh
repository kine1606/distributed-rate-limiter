#!/usr/bin/env bash
# Week 5 gate: kill a Redis master under load and record what each plan gets, second by second.
# Needs the fault cluster (3 masters with replicas): docker compose -f docker/docker-compose.faults.yml up -d
# FLUSHALL on its masters before every run: test data only. Output: results/<date>_fault_kill_master/.
#   bench/fault_kill_master.sh     env: RUNS=3  KILL_AT=10 (s into the measurement)  SECONDS_RUN=30
# Per run: one limiter (engine leased, defaults); open-loop load for 2 s of warm-up then SECONDS_RUN:
#   free  rule customer, cust:1..1000 (Free: fails closed)      1,000 checks/s
#   pay   rule customer, cust:pay:1..1000 (Pay: fails open)     5,000 checks/s
#   lease rule api_default (token bucket served from leases)    10,000 checks/s
# KILL_AT s in, the master owning slot 8000 is killed (docker kill: no clean shutdown); after the run
# it is started again and rejoins as a replica.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} kill_at=${KILL_AT:-10} secs=${SECONDS_RUN:-30} warm=2
out="results/$(date +%F)_fault_kill_master"
mkdir -p "$out"
seed=7101

masters() { redis-cli -p $seed cluster nodes | awk '/master/ && !/fail/ {split($2, a, "[:@]"); print a[2]}'; }
owner_of_slot() {  # port of the master serving slot $1
  redis-cli -p $seed cluster nodes | awk -v s="$1" '/master/ && !/fail/ {
    split($2, a, "[:@]"); for (i = 9; i <= NF; i++) { split($i, r, "-"); if (s >= r[1] && s <= (r[2] == "" ? r[1] : r[2])) print a[2] } }'
}
container_of_port() { docker ps --format '{{.Names}}' | grep -E "ratelimiter-faults-redis-$(($1 - 7100))-1"; }
wait_ok() { for _ in $(seq 100); do redis-cli -p $seed cluster info | grep -q cluster_state:ok && return 0; sleep 0.2; done; return 1; }

for r in $(seq "$runs"); do
  wait_ok
  for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
  REDIS_PORT=$seed tools/seed_plans.sh 1000 >/dev/null
  victim_port=$(owner_of_slot 8000)
  victim=$(container_of_port "$victim_port")
  build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml 127.0.0.1:$seed lua leased >"$out/run$r.server.log" 2>&1 &
  sp=$!
  sleep 2
  lg() { build/release/bench/loadgen 127.0.0.1:50051 "$1" "$secs" 4 2000 1000 "$warm" 1 "$2" "$3"; }
  lg customer 1000 cust: >"$out/run$r.free.json" &
  lf=$!
  lg customer 5000 cust:pay: >"$out/run$r.pay.json" &
  lp=$!
  lg api_default 10000 cust: >"$out/run$r.lease.json" &
  ll=$!
  sleep "$(echo "0.1 + $warm + $kill_at" | bc)"
  docker kill "$victim" >/dev/null
  echo "run $r: killed $victim (master on $victim_port) at ${kill_at}s" | tee "$out/run$r.kill.txt"
  wait $lf $lp $ll || true
  curl -s 127.0.0.1:9100/metrics >"$out/run$r.metrics.txt" || true
  kill $sp
  wait $sp 2>/dev/null || true
  redis-cli -p $seed cluster nodes | awk '{split($2, a, "@"); print a[1], $3, $9}' | sort >"$out/run$r.nodes_after.txt"
  docker start "$victim" >/dev/null  # rejoins as a replica of the promoted node
  sleep 3
done

python3 - "$out" "$runs" "$kill_at" <<'PY'
import json, sys
out, runs, kill_at = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
for r in range(1, runs + 1):
    print(f"run {r} (master killed at {kill_at} s):")
    for name in ("free", "pay", "lease"):
        d = json.load(open(f"{out}/run{r}.{name}.json"))
        f, tl = d["timeline_fields"], d["timeline"]
        bad = [i for i, row in enumerate(tl)
               if row[f.index("unavailable")] + row[f.index("unverified")] + row[f.index("error")] > 0]
        window = f"seconds {bad[0]} to {bad[-1]}" if bad else "none"
        o = d["outcomes"]
        print(f"  {name:5s} sent {d['sent_per_s']:.0f}/s, skipped {d['skipped']}, p99 {d['p99_ms']} ms; "
              f"ok {o['ok']}, unverified {o['unverified']}, unavailable {o['unavailable']}, "
              f"rate_limited {o['rate_limited']}, error {o['error']}; degraded {window}")
PY
