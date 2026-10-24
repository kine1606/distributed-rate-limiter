#!/usr/bin/env bash
# Week 7 bonus: Redis Functions (FCALL) against EVALSHA for the same token-bucket script, measured in
# Redis itself with redis-benchmark (the server's profile puts the Redis client at 4-9% of its CPU, so
# a gain would show here first). lua/token_bucket.lua is loaded unchanged, once as a script and once as
# the body of a function. Random keys over 100,000, 50 clients on 4 threads, without and with
# pipelining (16), alternating, RUNS runs each. FLUSHALL on the THROWAWAY test Redis (default 6380).
#   bench/redis_functions.sh     env: RUNS=5  PORT=6380
# Output: results/<date>_functions/summary.txt, then the table.
set -euo pipefail
cd "$(dirname "$0")/.."
runs=${RUNS:-5} port=${PORT:-6380}
out="results/$(date +%F)_functions"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); $(redis-cli -p "$port" info server | grep redis_version | tr -d '\r')" >"$sum"

sha=$(redis-cli -p "$port" SCRIPT LOAD "$(cat lua/token_bucket.lua)")
lib=$(printf '#!lua name=rl_bench\nlocal function token_bucket(KEYS, ARGV)\n%s\nend\nredis.register_function("token_bucket", token_bucket)\n' \
  "$(cat lua/token_bucket.lua)")
redis-cli -p "$port" FUNCTION LOAD REPLACE "$lib" >/dev/null
args=(1 "{b}:__rand_int__" 1000 500 1 2000)  # capacity, refill/s, requested, ttl ms
[ "$(redis-cli -p "$port" EVALSHA "$sha" 1 '{b}:x' 1000 500 1 2000 | head -1)" = "$(redis-cli -p "$port" FCALL token_bucket 1 '{b}:y' 1000 500 1 2000 | head -1)" ] ||
  { echo "EVALSHA and FCALL disagree" >&2; exit 1; }

for r in $(seq "$runs"); do
  for pipe in 1 16; do
    for cmd in evalsha fcall; do
      redis-cli -p "$port" FLUSHALL >/dev/null  # FLUSHALL keeps scripts and functions
      if [ "$cmd" = evalsha ]; then c=(EVALSHA "$sha" "${args[@]}"); else c=(FCALL token_bucket "${args[@]}"); fi
      rps=$(redis-benchmark -p "$port" -n 500000 -c 50 --threads 4 -P "$pipe" -r 100000 --csv "${c[@]}" |
        tail -1 | cut -d, -f2 | tr -d '"')
      echo "{\"cmd\":\"$cmd\",\"pipeline\":$pipe,\"run\":$r,\"calls_per_s\":$rps}" | tee -a "$sum"
    done
  done
done

python3 - "$sum" <<'EOF'
import json, statistics as st, sys
cells = {}
for line in open(sys.argv[1]):
    if line.startswith("{"):
        d = json.loads(line)
        cells.setdefault((d["cmd"], d["pipeline"]), []).append(d["calls_per_s"])
print("\n| Pipeline | EVALSHA calls/s (range) | FCALL calls/s (range) | Change |")
print("| --- | --- | --- | --- |")
for p in (1, 16):
    e, f = cells[("evalsha", p)], cells[("fcall", p)]
    me, mf = st.median(e), st.median(f)
    print(f"| {p} | {me:,.0f} ({min(e):,.0f} to {max(e):,.0f}) | {mf:,.0f} ({min(f):,.0f} to {max(f):,.0f}) | {100 * (mf - me) / me:+.1f}% |")
EOF
