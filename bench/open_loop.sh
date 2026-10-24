#!/usr/bin/env bash
# Week 5 gate, second half: latency at fixed offered rates, measured open loop (bench/loadgen with a
# rate: latency from each call's scheduled time). One limiter (engine leased, defaults) on the dev
# cluster, which is flushed and seeded before every run: test data only.
#   bench/open_loop.sh     env: RUNS=3  REDIS_PORT=7001
# Output: results/<date>_open_loop/summary.txt, one JSON line per run.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_open_loop"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); loadgen open loop, 12 channels, 2 s warm-up, 15 s" >"$sum"

# cell: <rule> <rate> <keys> <key prefix>
cells=("api_default 10000 10000 cust:" "api_default 20000 10000 cust:" "api_default 30000 10000 cust:"
       "api_default 40000 10000 cust:" "customer 5000 10000 cust:pay:" "customer 10000 10000 cust:pay:"
       "customer 20000 10000 cust:pay:")
for r in $(seq "$runs"); do
  for cell in "${cells[@]}"; do
    set -- $cell
    for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
      redis-cli -p "$m" FLUSHALL >/dev/null
    done
    REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
    build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua leased \
      >"$out/$1_$2_r$r.server.log" 2>&1 &
    sp=$!
    sleep 2
    echo "{\"cell\":\"$1_$2\",\"run\":$r,\"result\":$(build/release/bench/loadgen 127.0.0.1:50051 "$1" 15 12 2000 "$3" 2 1 "$2" "$4")}" \
      | tee -a "$sum" | cut -c1-200
    kill $sp
    wait $sp 2>/dev/null || true
  done
done
echo "summary: $sum"
