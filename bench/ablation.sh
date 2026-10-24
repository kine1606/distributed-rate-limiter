#!/usr/bin/env bash
# Week 7, the ablation study: each mechanism switched off on its own, on the workload where it should
# matter, against the full system on the same workload; alternating, RUNS runs each, one instance,
# closed loop (12 connections x 16 in flight), pinned as in bench/run_matrix.sh. Switches: the strict
# engine (lease), RL_ABLATE (early refill, adaptive chunk, deny cache), RL_PLAN_TTL_MS=0 (plan cache),
# RL_BATCHER (batcher, off by default, so "off" is the default), a build with RL_LEASE_ALIGN=8
# (alignment, build/noalign). FLUSHALL on the THROWAWAY dev cluster before every run.
# For the deny cache, every Free customer starts the run out of quota (counter at the plan's 200):
# the cache acts only then, and a Free customer's burst (2 a second) cannot get there within a run.
#   bench/ablation.sh     env: RUNS=5  REDIS_PORT=7001  SERVER_CPUS=0-5  CLIENT_CPUS=6-11  ONLY=<mechanism>
# Output: results/<date>_ablation/summary[_<ONLY>].txt (one JSON line per run), then the table from every
# summary there, a later file's cells replacing an earlier one's.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-5} port=${REDIS_PORT:-7001} scpus=${SERVER_CPUS:-0-5} ccpus=${CLIENT_CPUS:-6-11}
out="results/$(date +%F)_ablation"
mkdir -p "$out"
sum="$out/summary${ONLY:+_$ONLY}.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); server cpus $scpus, client cpus $ccpus" >"$sum"

if [ ! -x build/noalign/rate_limiter_server ]; then  # the same server with leases aligned to 8 bytes, not 64
  cmake -S . -B build/noalign -G Ninja -DCMAKE_BUILD_TYPE=Release -DRL_BUILD_TESTS=OFF -DRL_BUILD_BENCH=OFF \
    -DCMAKE_CXX_FLAGS=-DRL_LEASE_ALIGN=8 >/dev/null
  cmake --build build/noalign --target rate_limiter_server >/dev/null
fi

lport=30061
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }

exhaust() {  # Free customers cust:1..10000 out of today's quota; --pipe as tools/seed_plans.sh
  local day cmds
  day=$(($(redis-cli -p "$port" TIME | head -1) / 86400))
  cmds=$(mktemp)
  seq 10000 | awk -v d="$day" '{print "HSET {cust:" $1 "}:daily_quota:q day " d " used 200 overage_used 0"}' >"$cmds"
  for m in $(masters); do redis-cli -p "$m" --pipe <"$cmds" >/dev/null 2>&1 || true; done
  rm -f "$cmds"
  [ "$(redis-cli -c -p "$port" hget '{cust:10000}:daily_quota:q' used)" = 200 ] || { echo "exhaust failed" >&2; exit 1; }
}

# run <mechanism> <with|without> <binary> <env> <engine> <rule> <keys> <prefix>; keys drawn Zipf
run() {
  local mech=$1 variant=$2 bin=$3 envs=$4 engine=$5 rule=$6 keys=$7 prefix=$8
  [ -n "${ONLY:-}" ] && [ "$ONLY" != "$mech" ] && return
  for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
  REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
  [ "$mech" = deny_cache ] && exhaust
  env RL_METRICS_ADDR=off $envs taskset -c "$scpus" "$bin" "127.0.0.1:$lport" config/rules.yaml "127.0.0.1:$port" lua \
    "$engine" >/dev/null 2>&1 &
  sp=$!
  for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
  local res
  res=$(taskset -c "$ccpus" build/release/bench/loadgen "127.0.0.1:$lport" "$rule" 12 12 16 "$keys" 3 1 0 "$prefix" zipf)
  kill "$sp"
  wait "$sp" 2>/dev/null || true
  sp=
  echo "{\"mechanism\":\"$mech\",\"variant\":\"$variant\",\"result\":$res}" | tee -a "$sum" | cut -c1-120
}

full=build/release/rate_limiter_server
for r in $(seq "$runs"); do
  run lease with "$full" "" leased api_default 1000000 cust:
  run lease without "$full" "" strict api_default 1000000 cust:
  run early_refill with "$full" "" leased api_default 1000000 cust:
  run early_refill without "$full" "RL_ABLATE=early_refill" leased api_default 1000000 cust:
  run adaptive_chunk with "$full" "" leased api_default 1000000 cust:
  run adaptive_chunk without "$full" "RL_ABLATE=adaptive_chunk" leased api_default 1000000 cust:
  run alignment with "$full" "" leased api_default 1000000 cust:
  run alignment without build/noalign/rate_limiter_server "" leased api_default 1000000 cust:
  run batcher without "$full" "" strict api_default 1000000 cust:
  run batcher with "$full" "RL_BATCHER=adaptive" strict api_default 1000000 cust:
  run deny_cache with "$full" "" leased customer 10000 cust:
  run deny_cache without "$full" "RL_ABLATE=deny_cache" leased customer 10000 cust:
  run plan_cache with "$full" "" leased customer 10000 cust:pay:
  run plan_cache without "$full" "RL_PLAN_TTL_MS=0" leased customer 10000 cust:pay:
done

python3 - "$out"/summary*.txt <<'EOF'
import json, os, statistics as st, sys
cells = {}
for path in sorted(sys.argv[1:], key=os.path.getmtime):
    mine = {}
    for line in open(path):
        if line.startswith("{"):
            d = json.loads(line)
            mine.setdefault((d["mechanism"], d["variant"]), []).append(d["result"])
    cells.update(mine)
print("\n| Mechanism | Workload | With: checks/s, p99 | Without: checks/s, p99 | Throughput change without it |")
print("| --- | --- | --- | --- | --- |")
work = {"lease": "api_default, Zipf over 1M keys", "early_refill": "same", "adaptive_chunk": "same", "alignment": "same",
        "batcher": "strict path, Zipf over 1M keys", "deny_cache": "customer rule, Free plans out of quota, Zipf over 10k",
        "plan_cache": "customer rule, Pay plans, Zipf over 10k"}
for mech in work:
    w, wo = cells.get((mech, "with"), []), cells.get((mech, "without"), [])
    if not w or not wo:
        continue
    cw, co = st.median(x["checks_per_s"] for x in w), st.median(x["checks_per_s"] for x in wo)
    pw, po = st.median(x["p99_ms"] for x in w), st.median(x["p99_ms"] for x in wo)
    print(f"| {mech.replace('_', ' ')} | {work[mech]} | {cw:,.0f}, {pw:.2f} ms | {co:,.0f}, {po:.2f} ms | {100 * (co - cw) / cw:+.1f}% |")
EOF
