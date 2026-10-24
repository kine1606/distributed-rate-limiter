#!/usr/bin/env bash
# Week 7: where the server's CPU goes under load. One instance under the closed loop (12 connections x
# 16 in flight, Zipf keys), sampled by perf at 999 Hz for 10 s with frame-pointer call graphs, from a
# build of the same server with frame pointers (build/profile; Ubuntu 24.04's libraries keep theirs).
# perf_event_paranoid=2 allows user-space samples only, so time in the kernel shows as its absence.
# Per workload: a flamegraph (.svg), the top functions by self time, and self time by category.
# FLUSHALL on the THROWAWAY dev cluster before each workload.
#   bench/profile.sh     env: REDIS_PORT=7001  SERVER_CPUS=0-5  CLIENT_CPUS=6-11  PERF=<perf binary>
#                             FLAMEGRAPH=<github.com/brendangregg/FlameGraph checkout>
# Output: results/<date>_profile/
set -euo pipefail
cd "$(dirname "$0")/.."
port=${REDIS_PORT:-7001} scpus=${SERVER_CPUS:-0-5} ccpus=${CLIENT_CPUS:-6-11}
perf=${PERF:-$(ls /usr/lib/linux-tools-*/perf | tail -1)}
fg=${FLAMEGRAPH:-$HOME/.local/share/FlameGraph}
out="results/$(date +%F)_profile"
mkdir -p "$out"

cmake -S . -B build/profile -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF \
  "-DCMAKE_CXX_FLAGS=-fno-omit-frame-pointer -mno-omit-leaf-frame-pointer -g" >/dev/null
cmake --build build/profile --target rate_limiter_server >/dev/null

lport=30071
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }

# workload <name> <engine> <rule> <keys> <prefix>
workload() {
  local name=$1 engine=$2 rule=$3 keys=$4 prefix=$5
  for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
  REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
  RL_METRICS_ADDR=off taskset -c "$scpus" build/profile/rate_limiter_server "127.0.0.1:$lport" config/rules.yaml \
    "127.0.0.1:$port" lua "$engine" >/dev/null 2>&1 &
  sp=$!
  for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
  taskset -c "$ccpus" build/release/bench/loadgen "127.0.0.1:$lport" "$rule" 12 12 16 "$keys" 3 1 0 "$prefix" zipf \
    >"$out/$name.loadgen.json" &
  local lp=$!
  sleep 4
  "$perf" record -q -F 999 -g --call-graph fp -e cpu-clock:u -p "$sp" -o "$out/$name.perf.data" -- sleep 10 2>/dev/null
  wait "$lp"
  kill "$sp"
  wait "$sp" 2>/dev/null || true
  sp=
  "$perf" script -i "$out/$name.perf.data" 2>/dev/null | "$fg/stackcollapse-perf.pl" >"$out/$name.folded"
  "$fg/flamegraph.pl" --title "$name: $(python3 -c "import json; print(json.load(open('$out/$name.loadgen.json'))['checks_per_s'])") checks/s" \
    "$out/$name.folded" >"$out/$name.svg"
  "$perf" report -i "$out/$name.perf.data" --no-children --sort sym --stdio 2>/dev/null | grep -v '^#' | grep -v '^$' |
    head -25 >"$out/$name.top.txt"
  tools/profile_summary.py "$out/$name.folded" | tee "$out/$name.summary.txt"
  rm -f "$out/$name.perf.data"
}

workload leased leased api_default 1000000 cust:
workload customer leased customer 10000 cust:pay:
echo "flamegraphs: $out/*.svg"
