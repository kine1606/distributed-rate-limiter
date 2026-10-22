#!/usr/bin/env bash
# Week 5, SIGHUP reload (src/config/README.md): reload the rules three times under open-loop load and
# count failed calls, which must be 0. One limiter (engine leased) on the dev cluster, flushed and
# seeded before each pass: test data only. Each run has a control pass (same load, no reload) for the
# latency it costs. A probe rule shows each reload taking effect:
#   before any reload  reload_probe is unknown (INVALID_ARGUMENT, so the probe counts errors)
#   reload 1           reload_probe added: 5 tokens, 1 a second
#   reload 2           its bucket raised to 100,000 tokens, 100,000 a second
#   reload 3           a broken file: rejected, the rules of reload 2 stay live
#   bench/reload_under_load.sh     env: RUNS=3  RATE=10000  REDIS_PORT=7001
# Output: results/<date>_reload/summary.txt, one JSON line per load pass and per probe.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-3} rate=${RATE:-10000} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_reload"
mkdir -p "$out"
sum="$out/summary.txt"
rules="$out/rules.yaml"  # the file the server loads and reloads
echo "# $0 RUNS=$runs RATE=$rate at commit $(git rev-parse --short HEAD); open loop, 12 channels, 2 s warm-up, 24 s" >"$sum"

probe_rule() {  # <limit> <refill per second>
  printf '\n  reload_probe:\n    algorithm: token_bucket\n    limit: %s\n    refill_per_sec: %s\n' "$1" "$2"
  printf '    strict: false\n    failure_policy: fail_open\n'
}
probe() {  # <run> <label>: one key, one call at a time, 1 s
  echo "{\"run\":$1,\"probe\":\"$2\",\"result\":$(build/release/bench/loadgen 127.0.0.1:50051 reload_probe 1 1 1 1 0 1)}" \
    | tee -a "$sum" | cut -c1-200
}

# pass <run> <control | reload>
pass() {
  for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
    redis-cli -p "$m" FLUSHALL >/dev/null
  done
  REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
  cp config/rules.yaml "$rules"
  build/release/rate_limiter_server 127.0.0.1:50051 "$rules" "127.0.0.1:$port" lua leased \
    >"$out/$2_r$1.server.log" 2>&1 &
  local sp=$!
  sleep 2
  build/release/bench/loadgen 127.0.0.1:50051 api_default 24 12 2000 10000 2 1 "$rate" cust: >"$out/$2_r$1.load.json" &
  local lp=$!
  if [ "$2" = reload ]; then
    sleep 4 && probe "$1" before_reload
    { cat config/rules.yaml; probe_rule 5 1; } >"$rules" && kill -HUP $sp
    sleep 4 && probe "$1" after_add
    { cat config/rules.yaml; probe_rule 100000 100000; } >"$rules" && kill -HUP $sp
    sleep 4 && probe "$1" after_raise
    printf 'rules:\n  broken:\n    algorithm: no_such_algorithm\n' >"$rules" && kill -HUP $sp
    sleep 4 && probe "$1" after_broken_file
  fi
  wait $lp
  echo "{\"run\":$1,\"pass\":\"$2\",\"result\":$(cat "$out/$2_r$1.load.json")}" | tee -a "$sum" | cut -c1-260
  kill $sp
  wait $sp 2>/dev/null || true
  grep -h "reload" "$out/$2_r$1.server.log" | sed "s/^/# r$1 /" >>"$sum" || true
}

for r in $(seq "$runs"); do
  pass "$r" control
  pass "$r" reload
done
echo "summary: $sum"
