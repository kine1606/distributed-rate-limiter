#!/usr/bin/env bash
# Batcher comparison (ROADMAP Week 3) on the strict engine, where every check is one Redis script
# call: no batcher vs a fixed 200 us timer vs adaptive. Light load: p50/p99 at 1,000 checks/s
# offered (ghz --rps). Heavy load: throughput with the C++ load generator. Flushes the THROWAWAY
# dev cluster before every run. Output: results/<date>_batcher/summary.txt
#   bench/batcher_compare.sh     env: RUNS=3  REDIS_PORT=7001  LG="12 16"
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001} lg=${LG:-12 16}
out="results/$(date +%F)_batcher"
mkdir -p "$out"
sum="$out/summary.txt"
: >"$sum"
key='cust:{{randomInt 1 10000}}'

start_server() {  # start_server <mode> <log>; mode off|timer|adaptive
  for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
    redis-cli -p "$m" FLUSHALL >/dev/null
  done
  if [ "$1" = off ]; then
    build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua strict >"$2" 2>&1 &
  else
    RL_BATCHER=$1 build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua strict \
      >"$2" 2>&1 &
  fi
  sp=$!
  sleep 1
}

for r in $(seq "$runs"); do
  for mode in off timer adaptive; do
    start_server "$mode" "$out/light_${mode}_r$r.server.log"
    ghz --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check \
      -d "{\"key\":\"$key\",\"rule\":\"api_default\",\"cost\":1}" --rps 1000 -c 16 --connections 4 -z 5s \
      127.0.0.1:50051 >/dev/null
    ghz --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check \
      -d "{\"key\":\"$key\",\"rule\":\"api_default\",\"cost\":1}" --rps 1000 -c 16 --connections 4 -z 20s \
      -O json 127.0.0.1:50051 >"$out/light_${mode}_r$r.json"
    kill $sp
    wait $sp 2>/dev/null || true
    python3 - "$out/light_${mode}_r$r.json" "light_${mode}_r$r" <<'PY' | tee -a "$sum"
import json, sys
d = json.load(open(sys.argv[1]))
lat = sorted(x["latency"] for x in d["details"])
pct = lambda p: lat[min(len(lat) - 1, int(p / 100 * len(lat)))] / 1e6
print(f"{sys.argv[2]}: rps={d['rps']:.0f} p50={pct(50):.3f}ms p99={pct(99):.3f}ms codes={d['statusCodeDistribution']}")
PY
    gzip -f "$out/light_${mode}_r$r.json"

    start_server "$mode" "$out/heavy_${mode}_r$r.server.log"
    # shellcheck disable=SC2086
    echo "heavy_${mode}_r$r: $(build/release/bench/loadgen 127.0.0.1:50051 api_default 20 $lg 10000 5)" | tee -a "$sum"
    kill $sp
    wait $sp 2>/dev/null || true
  done
done
echo "summary: $sum"
