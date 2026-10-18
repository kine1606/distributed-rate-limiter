#!/usr/bin/env bash
# Week 3 gate measurements against the THROWAWAY dev cluster (flushed before every run):
#   1. leased vs strict on the Week 2 ghz command (c=1024, 10 s warm-up, 30 s), plus lease hit ratio
#   2. every engine on the faster C++ load generator, as a ratio to allow_all (gRPC alone)
#   3. p99 at 10,000 checks/s offered (ghz --rps), leased and allow_all as the control
# 5 runs per cell. Output: results/<date>_week3_gate/ (one line per run in summary.txt).
#   bench/week3_gate.sh     env: RUNS=5  REDIS_PORT=7001  LG="12 16" (loadgen channels, in flight per channel)
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-5} port=${REDIS_PORT:-7001} lg=${LG:-12 16}
out="results/$(date +%F)_week3_gate"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs REDIS_PORT=$port LG=\"$lg\" at commit $(git rev-parse --short HEAD); ghz $(ghz --version); loadgen options in each line" >"$sum"
key='cust:{{randomInt 1 10000}}'

flush_and_seed() {
  for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
    redis-cli -p "$m" FLUSHALL >/dev/null
  done
  REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
}

lease_stats() {  # hit ratio from the server log written at shutdown, over the calls ghz made
  local log=$1 calls=$2
  local parked
  parked=$(sed -n 's/.*lease stats: \([0-9]*\) checks waited.*/\1/p' "$log")
  python3 -c "print(f'hit_ratio={1 - $parked / $calls:.4f} parked=$parked calls=$calls')"
}

for r in $(seq "$runs"); do
  # 1. The Week 2 ghz command with the leased engine on the burst rule (ghz_run.sh flushes and seeds).
  bench/ghz_run.sh leased 1024 "$key" api_default "week3_gate/ghz_leased_c1024_r$r" | tee -a "$sum"
  sleep 1  # let the server finish writing its shutdown statistics
  calls=$(python3 -c "import gzip,json; print(json.load(gzip.open('$out/ghz_leased_c1024_r$r.json.gz'))['count'])")
  echo "ghz_leased_c1024_r$r: $(lease_stats "$out/ghz_leased_c1024_r$r.server.log" "$calls")" | tee -a "$sum"

  # 2. The C++ load generator, same keys, every engine.
  for cell in "allow_all api_default" "leased api_default" "strict api_default" "strict daily_quota"; do
    set -- $cell
    flush_and_seed
    build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua "$1" \
      >"$out/lg_$1_$2_r$r.server.log" 2>&1 &
    sp=$!
    sleep 1
    # shellcheck disable=SC2086
    echo "lg_$1_$2_r$r: $(build/release/bench/loadgen 127.0.0.1:50051 "$2" 20 $lg 10000 5)" | tee -a "$sum"
    kill $sp
    wait $sp 2>/dev/null || true
  done

  # 3. Latency at a fixed offered rate, with allow_all as the control: what gRPC and the laptop cost.
  for engine in allow_all leased; do
    flush_and_seed
    build/release/rate_limiter_server 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua "$engine" \
      >"$out/rps10k_${engine}_r$r.server.log" 2>&1 &
    sp=$!
    sleep 1
    ghz --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check \
      -d "{\"key\":\"$key\",\"rule\":\"api_default\",\"cost\":1}" --rps 10000 -c 64 --connections 4 -z 10s \
      127.0.0.1:50051 >/dev/null  # warm-up
    ghz --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check \
      -d "{\"key\":\"$key\",\"rule\":\"api_default\",\"cost\":1}" --rps 10000 -c 64 --connections 4 -z 30s \
      -O json 127.0.0.1:50051 >"$out/rps10k_${engine}_r$r.json"
    kill $sp
    wait $sp 2>/dev/null || true
    python3 - "$out/rps10k_${engine}_r$r.json" "rps10k_${engine}_r$r" <<'PY' | tee -a "$sum"
import json, sys
d = json.load(open(sys.argv[1]))
lat = sorted(x["latency"] for x in d["details"])
pct = lambda p: lat[min(len(lat) - 1, int(p / 100 * len(lat)))] / 1e6
print(f"{sys.argv[2]}: rps={d['rps']:.0f} p50={pct(50):.3f}ms p99={pct(99):.3f}ms p99.9={pct(99.9):.3f}ms "
      f"codes={d['statusCodeDistribution']}")
PY
    gzip -f "$out/rps10k_${engine}_r$r.json"
  done
done
echo "summary: $sum"
