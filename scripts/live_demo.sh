#!/usr/bin/env bash
# Live demo, about 90 s: watch the limiter decide at http://127.0.0.1:8090/ (dashboard/live.html) while
# this script plays a story against it:
#   1. normal      Pay and Free customers at 2,000 calls/s in all: everything allowed
#   2. flood       one Free customer (cust:1) sends 1,500/s: its burst (2/s) refuses most of them, then
#                  its daily quota (200) runs out and the deny cache answers locally
#   3. upgrade     tools/rlctl moves cust:1 to Pay: the next calls see the new plan
#   4. stall       one Redis master stops answering for 4 s: its third of the customers get their plan's
#                  policy (Pay allowed unverified, Free refused); the rest are untouched
#   5. calm        the flood stops
# FLUSHALL on the THROWAWAY dev cluster (7001-7003) first. Needs build/release (scripts/build.sh).
#   scripts/live_demo.sh      env: STALL=0 skips step 4, NO_OPEN=1 does not open a browser
set -euo pipefail
cd "$(dirname "$0")/.."
port=7001 grpc=50051 metrics=9100 view=8090
phase=$(mktemp)
pids=()
cleanup() {
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  rm -f "$phase"
}
trap cleanup EXIT
say() { echo "$*" | tee "$phase"; }
wait_port() { for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.1; done; echo "port $1 never opened" >&2; exit 1; }

redis-cli -p "$port" cluster info 2>/dev/null | grep -q cluster_state:ok ||
  { echo "dev Redis Cluster not running: docker compose -p ratelimiter-dev -f docker/docker-compose.dev.yml up -d" >&2; exit 1; }
masters=$(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}')
for m in $masters; do redis-cli -p "$m" FLUSHALL >/dev/null; done
REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
day=$(($(redis-cli -p "$port" TIME | head -1) / 86400))
redis-cli -c -p "$port" HSET "{cust:1}:daily_quota:q" day "$day" used 170 overage_used 0 >/dev/null  # 30 left today

RL_METRICS_ADDR=127.0.0.1:$metrics build/release/rate_limiter_server "127.0.0.1:$grpc" config/rules.yaml \
  "127.0.0.1:$port" lua leased >/dev/null 2>&1 &
pids+=($!)
wait_port "$grpc"
tools/live_dashboard.py --listen "127.0.0.1:$view" --metrics "http://127.0.0.1:$metrics/metrics" --phase-file "$phase" &
pids+=($!)
wait_port "$view"
url="http://127.0.0.1:$view/"
echo "open $url"
[ -z "${NO_OPEN:-}" ] && command -v powershell.exe >/dev/null &&
  powershell.exe -NoProfile -Command "Start-Process '$url'" >/dev/null 2>&1 || true

lg() {  # lg <seconds> <rate> <keys> <prefix>: open loop on the customer rule
  build/release/bench/loadgen "127.0.0.1:$grpc" customer "$1" 4 500 "$3" 0 1 "$2" "$4" >/dev/null 2>&1 &
  pids+=($!)
}
say "1. normal: 1,500 Pay and 500 Free calls a second over 2,000 customers, all within their limits"
lg 92 1500 1000 cust:pay:
lg 92 500 999 cust:1  # cust:11 .. cust:1999, Free, about 0.5 calls/s each
sleep 15
say "2. flood: one Free customer (cust:1) sends 1,500 calls/s; burst 2/s, 30 calls left of today's quota"
lg 55 1500 1 cust:
sleep 15  # 30 calls left at 2 a second
say "2b. out of quota: cust:1 has used today's 200; the deny cache now refuses it without asking Redis"
sleep 15
say "3. upgrade: tools/rlctl moves cust:1 to Pay (2,000 a day, burst 20/s); every instance applies it at once"
REDIS_PORT=$port tools/rlctl upgrade cust:1 --plan pay >/dev/null
sleep 15
if [ "${STALL:-1}" != 0 ]; then
  say "4. stall: Redis master :7002 stops answering for 4 s; its customers get their plan's policy"
  redis-cli -p 7002 CLIENT PAUSE 4000 ALL >/dev/null
  sleep 12
fi
say "5. calm: the flood is over"
wait "${pids[@]:2}" 2>/dev/null || true
say "done: no more traffic. Ctrl+C stops the limiter and the view"
wait "${pids[1]}"
