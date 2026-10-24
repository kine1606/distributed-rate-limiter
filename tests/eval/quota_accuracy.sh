#!/usr/bin/env bash
# Evaluation test 1, quota accuracy (docs/EVALUATION.md): 4 limiter instances, one Free and one Pay
# customer hammered with twice their daily allowance through all instances, 10% of the requests sent
# twice at once to two instances with the same request id. Pass, every run: the request ids that got
# an allowed answer, Redis's `used` and the usage events all equal the plan's allowance (Free 200;
# Pay 2,000 + 10% grace = 2,200), so nothing was admitted beyond the quota or counted twice.
# The burst limit is raised in both plans so that the quota, not the burst, is what binds.
# FLUSHALL on the THROWAWAY dev cluster before every run.
#   tests/eval/quota_accuracy.sh     env: RUNS=20  REDIS_PORT=7001
# Output: results/<date>_quota_accuracy/summary.txt, one JSON line per run, then the verdict.
set -euo pipefail
cd "$(dirname "$0")/../.."
runs=${RUNS:-20} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_quota_accuracy"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); 4 instances, engine leased, 10% sent twice at once" >"$sum"

# Ports below Linux's ephemeral range (32768 and up): there, a Redis connection of another instance
# can already hold the port as its local end, and the bind fails.
base=30070
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT
wait_port() { for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.1; done; return 1; }

for r in $(seq "$runs"); do
  for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
    redis-cli -p "$m" FLUSHALL >/dev/null
  done
  redis-cli -c -p "$port" HSET "{cust:eval:free}:plan" plan free daily 200 burst 100000 refill 100000 \
    overage hard extra 0 failure closed >/dev/null
  redis-cli -c -p "$port" HSET "{cust:eval:pay}:plan" plan pay daily 2000 burst 100000 refill 100000 \
    overage grace extra 200 failure open >/dev/null
  pids=() targets=()
  rm -f "$out"/usage_r"${r}"_*.jsonl  # the usage log appends
  for i in 1 2 3 4; do
    RL_USAGE_FILE="$out/usage_r${r}_$i.jsonl" RL_METRICS_ADDR=off build/release/rate_limiter_server \
      "127.0.0.1:$((base + i))" config/rules.yaml "127.0.0.1:$port" lua leased >"$out/server_r${r}_$i.log" 2>&1 &
    pids+=($!) targets+=("127.0.0.1:$((base + i))")
  done
  for i in 1 2 3 4; do wait_port $((base + i)); done
  client=$(RUN=$r python3 tests/eval/quota_client.py "$(IFS=,; echo "${targets[*]}")" cust:eval:free=400 cust:eval:pay=4400)
  kill "${pids[@]}"
  wait "${pids[@]}" 2>/dev/null || true  # SIGTERM flushes the usage events
  used_free=$(redis-cli -c -p "$port" HGET "{cust:eval:free}:daily_quota:q" used)
  used_pay=$(redis-cli -c -p "$port" HGET "{cust:eval:pay}:daily_quota:q" used)
  usage=$(python3 -c "
import json, sys
sys.path.insert(0, 'tools')
from usage_report import totals
lines = [l for p in sys.argv[1:] for l in open(p)]
print(json.dumps({c: t['allowed'] for c, t in totals(lines).items()}))" "$out"/usage_r"${r}"_*.jsonl)
  echo "{\"run\":$r,\"redis_used\":{\"cust:eval:free\":${used_free:-0},\"cust:eval:pay\":${used_pay:-0}},\"usage_allowed\":$usage,\"client\":$client}" \
    | tee -a "$sum" | cut -c1-220
done

python3 - "$sum" <<'EOF'
import json, sys
want = {"cust:eval:free": 200, "cust:eval:pay": 2200}
runs = [json.loads(l) for l in open(sys.argv[1]) if l.startswith("{")]
bad = 0
for r in runs:
    for c, n in want.items():
        got = (r["client"][c]["allowed_ids"], r["redis_used"][c], r["usage_allowed"].get(c, 0))
        if got != (n, n, n) or r["client"][c]["errors"]:
            bad += 1
            print(f"FAIL run {r['run']} {c}: allowed ids, Redis used, usage events = {got}, want {n}; errors {r['client'][c]['errors']}")
print(f"verdict: {len(runs) - bad if bad <= len(runs) else 0} of {len(runs)} runs exact" if not bad else f"verdict: {bad} failures")
EOF
