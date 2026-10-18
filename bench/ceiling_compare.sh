#!/usr/bin/env bash
# How far one laptop gets (the 200k checks/s question), and the limiter next to envoyproxy/ratelimit on
# the same machine with the same load generator (bench/loadgen, which speaks both APIs). FLUSHALL on
# the THROWAWAY dev cluster and on the throwaway Redis of the Envoy service before every run: never
# point it at Redis holding real data. Output: results/<date>_ceiling/summary.txt, one line per run.
#   bench/ceiling_compare.sh     env: RUNS=3  ENVOY_RL=<ratelimit binary>  ENVOY_REDIS_PORT=6390
# The Envoy service runs natively, like the limiter, from the official image at a pinned commit:
#   docker create --name rl-tmp envoyproxy/ratelimit:f4e76690 && docker cp rl-tmp:/bin/ratelimit "$ENVOY_RL"
#   docker rm rl-tmp && docker run -d --name rl-compare-redis -p 127.0.0.1:6390:6379 redis:7
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} port=7001 eport=${ENVOY_REDIS_PORT:-6390}
envoy_bin=${ENVOY_RL:?set ENVOY_RL to the envoyproxy/ratelimit binary}
out="results/$(date +%F)_ceiling"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); envoyproxy/ratelimit f4e76690; 12 s loadgen, 10 s sampled" >"$sum"

# Envoy config: one counter per customer, 500 a second, like api_default's refill rate.
cfg=$(mktemp -d)
trap 'rm -rf "$cfg"' EXIT
mkdir -p "$cfg/rl/config"
printf 'domain: bench\ndescriptors:\n  - key: cust\n    rate_limit: {unit: second, requests_per_unit: 500}\n' \
  >"$cfg/rl/config/config.yaml"

hz=$(getconf CLK_TCK)
ticks() { local t=0 p; for p in "$@"; do t=$((t + $(awk '{print $14+$15}' "/proc/$p/stat" 2>/dev/null || echo 0))); done; echo $t; }
flush() {
  for m in $(redis-cli -p $port cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
    redis-cli -p "$m" FLUSHALL >/dev/null
  done
  redis-cli -p "$eport" FLUSHALL >/dev/null
}

# run <label> <ours:ENGINE | envoy | envoy:cache> <instances> <rule> <channels> <in_flight> <keys> <batch>
run() {
  local label=$1 server=$2 n=$3 rule=$4 ch=$5 inf=$6 keys=$7 batch=$8 i
  flush
  local pids=() lps=() outs=()
  for i in $(seq 0 $((n - 1))); do
    if [[ $server == envoy* ]]; then
      local cache=0
      [[ $server == envoy:cache ]] && cache=67108864  # 64 MB of over-limit keys
      RUNTIME_ROOT=$cfg RUNTIME_SUBDIRECTORY=rl RUNTIME_WATCH_ROOT=false USE_STATSD=false LOG_LEVEL=warn \
        REDIS_SOCKET_TYPE=tcp REDIS_URL=127.0.0.1:$eport LOCAL_CACHE_SIZE_IN_BYTES=$cache \
        GRPC_PORT=$((50051 + i)) PORT=$((8080 + i)) DEBUG_PORT=$((6070 + i)) "$envoy_bin" >/dev/null 2>&1 &
    else
      build/release/rate_limiter_server 127.0.0.1:$((50051 + i)) config/rules.yaml 127.0.0.1:$port lua \
        "${server#ours:}" >/dev/null 2>&1 &
    fi
    pids+=($!)
  done
  sleep 1.5
  for i in $(seq 0 $((n - 1))); do
    outs+=("$(mktemp)")
    build/release/bench/loadgen 127.0.0.1:$((50051 + i)) "$rule" 12 "$ch" "$inf" "$keys" 3 "$batch" >"${outs[i]}" &
    lps+=($!)
  done
  sleep 3  # the generators' warm-up
  local s0 g0 t0 s1 g1 t1
  s0=$(ticks "${pids[@]}") g0=$(ticks "${lps[@]}") t0=$(date +%s.%N)
  sleep 10  # inside the generators' 12 s measurement
  s1=$(ticks "${pids[@]}") g1=$(ticks "${lps[@]}") t1=$(date +%s.%N)
  for i in "${lps[@]}"; do wait "$i"; done
  kill "${pids[@]}"
  wait "${pids[@]}" 2>/dev/null || true
  python3 - "$label" "$server" "$n" "$s0" "$s1" "$g0" "$g1" "$t0" "$t1" "$hz" "${outs[@]}" <<'PY' | tee -a "$sum"
import json, sys
label, server, n = sys.argv[1:4]
s0, s1, g0, g1, t0, t1, hz = map(float, sys.argv[4:11])
ds = [json.load(open(f)) for f in sys.argv[11:]]
el = t1 - t0
print(json.dumps({"label": label, "server": server, "instances": int(n), "rule": ds[0]["rule"],
                  "channels": ds[0]["channels"], "in_flight": ds[0]["in_flight"], "keys": ds[0]["keys"],
                  "batch": ds[0]["batch"], "checks_per_s": round(sum(d["checks_per_s"] for d in ds)),
                  "calls_per_s": round(sum(d["calls_per_s"] for d in ds)),
                  "allowed_per_s": round(sum(d["allowed"] / d["seconds"] for d in ds)),
                  "errors": sum(d["errors"] for d in ds), "p50_ms": max(d["p50_ms"] for d in ds),
                  "p99_ms": max(d["p99_ms"] for d in ds), "server_cpu_pct": round((s1 - s0) / hz / el * 100),
                  "loadgen_cpu_pct": round((g1 - g0) / hz / el * 100)}))
PY
  rm -f "${outs[@]}"
}

for r in $(seq "$runs"); do
  run "leased_1x_r$r" ours:leased 1 api_default 12 16 10000 1
  run "leased_4x_r$r" ours:leased 4 api_default 3 16 10000 1
  run "allow_all_4x_r$r" ours:allow_all 4 api_default 3 16 10000 1
  run "leased_batch10_r$r" ours:leased 1 api_default 12 16 10000 10
  run "leased_batch100_r$r" ours:leased 1 api_default 12 16 10000 100
  run "leased_batch10_100keys_r$r" ours:leased 1 api_default 12 16 100 10  # limiting: 100 x 500/s allowed
  run "strict_12conn_r$r" ours:strict 1 api_default 12 16 10000 1
  run "strict_1conn_r$r" ours:strict 1 api_default 1 192 10000 1
  run "envoy_1x_r$r" envoy 1 envoy:bench 12 16 10000 1
  run "envoy_batch10_r$r" envoy 1 envoy:bench 12 16 10000 10
  run "envoy_cache_100keys_r$r" envoy:cache 1 envoy:bench 12 16 100 1
  run "leased_100keys_r$r" ours:leased 1 api_default 12 16 100 1
done
echo "summary: $sum"
