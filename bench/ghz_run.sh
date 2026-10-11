#!/usr/bin/env bash
# One load-test cell: start the limiter with <engine> against a THROWAWAY Redis, run ghz, save the
# raw JSON plus server/Redis/ghz CPU to results/.  Never point this at a Redis holding real data:
# it runs FLUSHALL first.
#   bench/ghz_run.sh <engine> <concurrency> <key template> <rule> <label>
#   env: GHZ=ghz  SERVER=build/release/rate_limiter_server  REDIS_PORT=6380  DUR=10 (seconds)
# Example: bench/ghz_run.sh leased 128 'user:{{randomInt 1 10000}}' api_default leased_c128
set -euo pipefail
cd "$(dirname "$0")/.."
engine=$1 c=$2 key=$3 rule=$4 label=$5
GHZ=${GHZ:-ghz} SERVER=${SERVER:-build/release/rate_limiter_server} port=${REDIS_PORT:-6380} dur=${DUR:-10}
out="results/$(date +%F)_$label"

redis-cli -p "$port" FLUSHALL >/dev/null
"$SERVER" 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua "$engine" >"$out.server.log" 2>&1 &
sp=$!
trap 'kill $sp 2>/dev/null || true' EXIT
sleep 1
rp=$(redis-cli -p "$port" info server | awk -F: '/process_id/{print $2}' | tr -d '\r')
ticks() { awk '{print $14+$15}' "/proc/$1/stat"; }
s0=$(ticks $sp) r0=$(ticks "$rp") t0=$(date +%s.%N)
cmd=("$GHZ" --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check
     -d "{\"key\":\"$key\",\"rule\":\"$rule\",\"cost\":1}" -c "$c" --connections 4 -z "${dur}s" -O json 127.0.0.1:50051)
TIMEFORMAT='%U %S'; { time "${cmd[@]}" >"$out.json"; } 2>"$out.ghz_cpu"
t1=$(date +%s.%N) s1=$(ticks $sp) r1=$(ticks "$rp")
hz=$(getconf CLK_TCK)
python3 - "$out" "$label" "$engine" "$*" "${cmd[*]}" "$s0" "$s1" "$r0" "$r1" "$t0" "$t1" "$hz" <<'PY'
import json, sys
out, label, engine, args, cmd = sys.argv[1:6]
s0, s1, r0, r1, t0, t1, hz = map(float, sys.argv[6:13])
d = json.load(open(out + ".json")); el = t1 - t0
ghz_cpu = sum(map(float, open(out + ".ghz_cpu").read().split())) / el * 100
lat = {x["percentage"]: x["latency"] / 1e6 for x in d["latencyDistribution"]}
line = (f"{label}: rps={d['rps']:.0f} p50={lat[50]:.2f}ms p99={lat[99]:.2f}ms "
        f"codes={d['statusCodeDistribution']} cpu%: server={(s1-s0)/hz/el*100:.0f} "
        f"redis={(r1-r0)/hz/el*100:.0f} ghz={ghz_cpu:.0f}")
print(line)
with open(out + ".txt", "w") as f:
    f.write(line + "\nengine: " + engine + "\nscript args: " + args + "\nghz: " + cmd + "\n")
PY
