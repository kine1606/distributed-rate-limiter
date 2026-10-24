#!/usr/bin/env bash
# Evaluation tests 4 and 12, the burst accuracy bound and the bound model (docs/EVALUATION.md). One
# hot key of a leased token bucket (limit 1,000, refill 100 a second) through 4 limiter instances,
# at chunk caps of 1%, 5% and 10% of the limit (C = 10, 50, 100; worst case N x C = 40, 200, 400).
# Two demands, each at 500 calls a second per instance (20 times the refill rate):
#   idle_burst  one check per instance fills its lease, 12 s of silence refill the bucket, 3 s of
#               load: the worst case the proof allows
#   steady      10 s of load from a full bucket and empty leases
# Each run's over-admission is measured on the allowed answers (receive times at the client), and the
# same demand (send times) is replayed through tests/eval/bound_model.py for the prediction.
# FLUSHALL on the THROWAWAY dev cluster before every run.
#   tests/eval/burst_accuracy.sh     env: RUNS=5  REDIS_PORT=7001
# Output: results/<date>_burst_accuracy/summary.txt, one JSON line per run.
set -euo pipefail
cd "$(dirname "$0")/../.."
runs=${RUNS:-5} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_burst_accuracy"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); 4 instances, engine leased, limit 1000, refill 100/s" >"$sum"

base=30080  # below Linux's ephemeral range (see tests/eval/quota_accuracy.sh)
pids=()
trap 'kill "${pids[@]}" 2>/dev/null || true' EXIT
wait_port() { for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0; sleep 0.1; done; return 1; }

for r in $(seq "$runs"); do
  for pct in 1 5 10; do
    for scenario in idle_burst steady; do
      for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
        redis-cli -p "$m" FLUSHALL >/dev/null
      done
      rules="$out/rules_$pct.yaml"
      { cat config/rules.yaml
        printf '\n  bound_test:\n    algorithm: token_bucket\n    limit: 1000\n    refill_per_sec: 100\n'
        printf '    strict: false\n    failure_policy: fail_closed\n    lease_chunk_pct: %s\n' "$pct"; } >"$rules"
      pids=() targets=()
      for i in 1 2 3 4; do
        RL_METRICS_ADDR=off build/release/rate_limiter_server "127.0.0.1:$((base + i))" "$rules" "127.0.0.1:$port" lua \
          leased >"$out/server_${scenario}_${pct}_r${r}_$i.log" 2>&1 &
        pids+=($!) targets+=("127.0.0.1:$((base + i))")
      done
      for i in 1 2 3 4; do wait_port $((base + i)); done
      events="$out/${scenario}_${pct}_r$r.json"
      if [ "$scenario" = idle_burst ]; then args=(1 12 3 500); else args=(0 0 10 500); fi
      python3 tests/eval/burst_client.py "$(IFS=,; echo "${targets[*]}")" bound_test bound:hot "${args[@]}" "$events"
      kill "${pids[@]}"
      wait "${pids[@]}" 2>/dev/null || true
      python3 - "$events" "$pct" "$scenario" "$r" <<'EOF' | tee -a "$sum"
import json, sys
sys.path.insert(0, "tests/eval")
from bound_model import exact, over_admission, simulate
events, pct, scenario, run = sys.argv[1], int(sys.argv[2]), sys.argv[3], int(sys.argv[4])
n, limit, rate = 4, 1000, 100
cap = limit * pct // 100
d = json.load(open(events))
arrivals = sorted((c[1], c[0]) for c in d["calls"])
allowed = [c[2] for c in d["calls"] if c[3]]
predicted = {f"{rtt * 1000:g}ms": simulate(arrivals, n, limit, rate, cap, rtt) for rtt in (0.0005, 0.001, 0.002)}
print(json.dumps({
    "pct": pct, "chunk": cap, "scenario": scenario, "run": run, "calls": len(d["calls"]), "errors": d["errors"],
    "send_lag_p99_ms": round(d["send_lag_p99_s"] * 1000, 2), "allowed": len(allowed),
    "measured_over": round(over_admission(allowed, limit, rate), 1),
    "predicted_over": {k: round(over_admission([t for t, _ in v], limit, rate), 1) for k, v in predicted.items()},
    "predicted_allowed": len(predicted["1ms"]), "exact_allowed": len(exact(arrivals, limit, rate)), "worst": n * cap}))
EOF
    done
  done
done
echo "summary: $sum"
