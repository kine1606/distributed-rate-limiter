#!/usr/bin/env bash
# Two server binaries in alternating runs on the same workloads (CLAUDE.md: every speed-up measured
# before and after): leased api_default over Zipf 1M keys, the customer rule over Zipf 10k Pay
# customers; one instance, closed loop 12 x 16, pinned as bench/run_matrix.sh. FLUSHALL on the
# THROWAWAY dev cluster before every run.
#   bench/ab_compare.sh <label> <binary A> <binary B>     env: RUNS=5  REDIS_PORT=7001  SERVER_CPUS  CLIENT_CPUS
# Output: results/<date>_ab_<label>/summary.txt, then the table on stdout.
set -euo pipefail
cd "$(dirname "$0")/.."
label=$1 a=$2 b=$3
runs=${RUNS:-5} port=${REDIS_PORT:-7001} scpus=${SERVER_CPUS:-0-5} ccpus=${CLIENT_CPUS:-6-11}
out="results/$(date +%F)_ab_$label"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 $label A=$a B=$b RUNS=$runs at commit $(git rev-parse --short HEAD)" >"$sum"
lport=30081
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }

run() {  # run <variant> <binary> <rule> <keys> <prefix>
  for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
  REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
  RL_METRICS_ADDR=off taskset -c "$scpus" "$2" "127.0.0.1:$lport" config/rules.yaml "127.0.0.1:$port" lua leased \
    >/dev/null 2>&1 &
  sp=$!
  for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
  local res
  res=$(taskset -c "$ccpus" build/release/bench/loadgen "127.0.0.1:$lport" "$3" 12 12 16 "$4" 3 1 0 "$5" zipf)
  kill "$sp"
  wait "$sp" 2>/dev/null || true
  sp=
  echo "{\"variant\":\"$1\",\"rule\":\"$3\",\"result\":$res}" | tee -a "$sum" | cut -c1-110
}

for r in $(seq "$runs"); do
  for v in A B; do
    bin=$a
    [ "$v" = B ] && bin=$b
    run "$v" "$bin" api_default 1000000 cust:
    run "$v" "$bin" customer 10000 cust:pay:
  done
done

python3 - "$sum" <<'EOF'
import json, statistics as st, sys
cells = {}
for line in open(sys.argv[1]):
    if line.startswith("{"):
        d = json.loads(line)
        cells.setdefault((d["rule"], d["variant"]), []).append(d["result"])
print("\n| Workload | A: checks/s (range), p99 | B: checks/s (range), p99 | Change |")
print("| --- | --- | --- | --- |")
for rule in ("api_default", "customer"):
    def cell(v):
        rs = cells[(rule, v)]
        c = [x["checks_per_s"] for x in rs]
        return st.median(c), f"{st.median(c):,.0f} ({min(c):,.0f} to {max(c):,.0f}), {st.median(x['p99_ms'] for x in rs):.2f} ms"
    (ma, ta), (mb, tb) = cell("A"), cell("B")
    print(f"| {rule} | {ta} | {tb} | {100 * (mb - ma) / ma:+.1f}% |")
EOF
