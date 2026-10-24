#!/usr/bin/env bash
# Week 7, the benchmark matrix (bench/README.md): throughput and latency of each engine on 1, 2 and 4
# instances, over three key distributions, closed loop at the plateau (192 calls in flight in all,
# split over the instances), RUNS runs per cell. One command reruns the whole matrix; tools/matrix_tables.py
# turns its summary into the Chapter 5 tables.
#   allow_all   no limiter: the gRPC ceiling
#   strict      api_default checked in Redis on every call
#   leased      api_default from leases, chunk cap 1%, 5% and 10% of the limit (lease_chunk_pct)
#   customer    the customer rule (burst then daily quota) for Pay customers cust:pay:1..1000000
# Keys: uniform over 1,000,000, Zipf (exponent 0.99) over 1,000,000, one hot key.
# Servers are pinned to SERVER_CPUS and load generators to CLIENT_CPUS; Redis runs in Docker, unpinned.
# FLUSHALL on the THROWAWAY dev cluster before every run of the first three engines; the customer
# cells run last, on 1,000,000 Pay plans seeded once.
#   bench/run_matrix.sh     env: RUNS=5  REDIS_PORT=7001  SERVER_CPUS=0-5  CLIENT_CPUS=6-11  ONLY=<engine>
# Output: results/<date>_matrix/summary.txt, one JSON line per run.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-5} port=${REDIS_PORT:-7001} scpus=${SERVER_CPUS:-0-5} ccpus=${CLIENT_CPUS:-6-11}
out="results/$(date +%F)_matrix"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); server cpus $scpus, client cpus $ccpus; $(nproc) threads" >"$sum"

base=30100  # below Linux's ephemeral range (see tests/eval/quota_accuracy.sh)
hz=$(getconf CLK_TCK)
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT
ticks() { local t=0 p; for p in "$@"; do t=$((t + $(awk '{print $14+$15}' "/proc/$p/stat" 2>/dev/null || echo 0))); done; echo $t; }
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }
flush() { for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done; }
for pct in 1 5 10; do  # api_default with each chunk cap
  sed "/^  api_default:/a\\    lease_chunk_pct: $pct" config/rules.yaml >"$out/rules_cap$pct.yaml"
done

# cell <engine> <server engine> <rules> <rule> <instances> <dist> <prefix> <label extras>
cell() {
  local engine=$1 server=$2 rules=$3 rule=$4 n=$5 dist=$6 prefix=$7 extra=$8 r i
  local keys=1000000
  [ "$dist" = hot ] && keys=1
  local kdist=$dist
  [ "$dist" = hot ] && kdist=uniform
  for r in $(seq "$runs"); do
    [ "$engine" != customer ] && flush
    pids=()
    local lps=() outs=()
    for i in $(seq 1 "$n"); do
      RL_METRICS_ADDR=off taskset -c "$scpus" build/release/rate_limiter_server "127.0.0.1:$((base + i))" "$rules" \
        "127.0.0.1:$port" lua "$server" >/dev/null 2>&1 &
      pids+=($!)
    done
    for i in $(seq 1 "$n"); do
      for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$((base + i))") 2>/dev/null && break; sleep 0.1; done
    done
    for i in $(seq 1 "$n"); do
      outs+=("$(mktemp)")
      taskset -c "$ccpus" build/release/bench/loadgen "127.0.0.1:$((base + i))" "$rule" 12 $((12 / n)) 16 "$keys" 3 1 0 \
        "$prefix" "$kdist" >"${outs[$((i - 1))]}" &
      lps+=($!)
    done
    sleep 3.5  # the generators' warm-up
    local s0 g0 t0 s1 g1 t1
    s0=$(ticks "${pids[@]}") g0=$(ticks "${lps[@]}") t0=$(date +%s.%N)
    sleep 10  # inside the 12 s measurement
    s1=$(ticks "${pids[@]}") g1=$(ticks "${lps[@]}") t1=$(date +%s.%N)
    for i in "${lps[@]}"; do wait "$i"; done
    kill "${pids[@]}"
    wait "${pids[@]}" 2>/dev/null || true
    python3 - "$engine" "$n" "$dist" "$extra" "$r" "$s0" "$s1" "$g0" "$g1" "$t0" "$t1" "$hz" "${outs[@]}" <<'PY' | tee -a "$sum" | cut -c1-150
import json, sys
engine, n, dist, extra, run = sys.argv[1:6]
s0, s1, g0, g1, t0, t1, hz = map(float, sys.argv[6:13])
ds = [json.load(open(f)) for f in sys.argv[13:]]
el = t1 - t0
line = {"engine": engine, "instances": int(n), "dist": dist, "run": int(run),
        "checks_per_s": round(sum(d["checks_per_s"] for d in ds)),
        "allowed_per_s": round(sum(d["allowed"] / d["seconds"] for d in ds)),
        "unverified": sum(d.get("unverified", 0) for d in ds),
        "errors": sum(d["errors"] for d in ds), "p50_ms": max(d["p50_ms"] for d in ds),
        "p99_ms": max(d["p99_ms"] for d in ds), "server_cpu_pct": round((s1 - s0) / hz / el * 100),
        "loadgen_cpu_pct": round((g1 - g0) / hz / el * 100)}
if extra:
    line.update(json.loads(extra))
print(json.dumps(line))
PY
    rm -f "${outs[@]}"
  done
}

only=${ONLY:-}
for n in 1 2 4; do
  if [ -z "$only" ] || [ "$only" = allow_all ]; then
    cell allow_all allow_all config/rules.yaml api_default "$n" uniform cust: ""
  fi
  for dist in uniform zipf hot; do
    if [ -z "$only" ] || [ "$only" = strict ]; then
      cell strict strict config/rules.yaml api_default "$n" "$dist" cust: ""
    fi
    if [ -z "$only" ] || [ "$only" = leased ]; then
      for pct in 1 5 10; do
        cell leased leased "$out/rules_cap$pct.yaml" api_default "$n" "$dist" cust: "{\"chunk_pct\": $pct}"
      done
    fi
  done
done
if [ -z "$only" ] || [ "$only" = customer ]; then
  flush
  REDIS_PORT=$port tools/seed_plans.sh 1000000 >/dev/null
  for n in 1 2 4; do
    for dist in uniform zipf hot; do
      cell customer leased config/rules.yaml customer "$n" "$dist" cust:pay: ""
    done
  done
fi
echo "summary: $sum"
