#!/usr/bin/env bash
# Week 8: throughput against concurrency on one instance, the data for the Universal Scalability Law
# fit (tools/usl_fit.py). Closed loop at N calls in flight (N = 1 to 384; channels = min(N, 12)),
# for gRPC alone, the strict engine over Zipf 1M keys, and the leased engine over Zipf 10k keys
# (mostly lease hits); pinned as bench/run_matrix.sh, RUNS runs per point, FLUSHALL on the THROWAWAY
# dev cluster before every run.
#   bench/concurrency_sweep.sh     env: RUNS=3  REDIS_PORT=7001  SERVER_CPUS=0-5  CLIENT_CPUS=6-11
# Output: results/<date>_concurrency/summary.txt, one JSON line per run.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001} scpus=${SERVER_CPUS:-0-5} ccpus=${CLIENT_CPUS:-6-11}
out="results/$(date +%F)_concurrency"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); server cpus $scpus, client cpus $ccpus" >"$sum"
lport=30091
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }

for r in $(seq "$runs"); do
  for n in 1 2 4 8 12 24 48 96 192 384; do
    ch=$((n < 12 ? n : 12))
    for cell in "allow_all api_default 1000000" "strict api_default 1000000" "leased api_default 10000"; do
      set -- $cell
      for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
      RL_METRICS_ADDR=off taskset -c "$scpus" build/release/rate_limiter_server "127.0.0.1:$lport" config/rules.yaml \
        "127.0.0.1:$port" lua "$1" >/dev/null 2>&1 &
      sp=$!
      for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
      res=$(taskset -c "$ccpus" build/release/bench/loadgen "127.0.0.1:$lport" "$2" 8 "$ch" $((n / ch)) "$3" 2 1 0 cust: zipf)
      kill "$sp"
      wait "$sp" 2>/dev/null || true
      sp=
      echo "{\"engine\":\"$1\",\"n\":$n,\"run\":$r,\"result\":$res}" | tee -a "$sum" | cut -c1-100
    done
  done
done
