#!/usr/bin/env bash
# One load-test run: flush the THROWAWAY dev cluster, seed plans, start the limiter with <engine>,
# warm up, then measure with ghz. Saves the gzipped raw ghz JSON, the server log and a summary line
# (rps, p50/p99/p99.9, server/Redis/ghz CPU) to results/. Never point it at a cluster holding real
# data: it runs FLUSHALL on every master.
#   bench/ghz_run.sh <engine> <concurrency> <key template> <rule> <label>
#   env: GHZ=ghz  SERVER=build/release/rate_limiter_server  REDIS_PORT=7001 (dev cluster node)
#        WARM=10  DUR=30 (seconds)  REDIS_CONTAINERS="ratelimiter-dev-redis-1-1 ... -3-1" (for CPU)
# Example: bench/ghz_run.sh strict 64 'cust:{{randomInt 1 10000}}' daily_quota strict_c64_r1
set -euo pipefail
cd "$(dirname "$0")/.."
engine=$1 c=$2 key=$3 rule=$4 label=$5
GHZ=${GHZ:-ghz} SERVER=${SERVER:-build/release/rate_limiter_server} port=${REDIS_PORT:-7001}
warm=${WARM:-10} dur=${DUR:-30}
containers=${REDIS_CONTAINERS:-ratelimiter-dev-redis-1-1 ratelimiter-dev-redis-2-1 ratelimiter-dev-redis-3-1}
out="results/$(date +%F)_$label"

for m in $(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'); do
  redis-cli -p "$m" FLUSHALL >/dev/null
done
REDIS_PORT=$port tools/seed_plans.sh 10000 >/dev/null
"$SERVER" 127.0.0.1:50051 config/rules.yaml "127.0.0.1:$port" lua "$engine" >"$out.server.log" 2>&1 &
sp=$!
trap 'kill $sp 2>/dev/null || true' EXIT
sleep 1

cmd=("$GHZ" --insecure --proto proto/rate_limiter.proto --call ratelimiter.v1.RateLimiter.Check
     -d "{\"key\":\"$key\",\"rule\":\"$rule\",\"cost\":1}" -c "$c" --connections 4 -O json 127.0.0.1:50051)
"${cmd[@]}" -z "${warm}s" >/dev/null  # warm-up, discarded

ticks() { awk '{print $14+$15}' "/proc/$1/stat"; }
redis_usec() { for ct in $containers; do docker exec "$ct" awk '/usage_usec/{print $2}' /sys/fs/cgroup/cpu.stat; done | paste -sd+ | bc; }
s0=$(ticks $sp) r0=$(redis_usec) t0=$(date +%s.%N)
TIMEFORMAT='%U %S'; { time "${cmd[@]}" -z "${dur}s" >"$out.json"; } 2>"$out.ghz_cpu"
t1=$(date +%s.%N) s1=$(ticks $sp) r1=$(redis_usec)
hz=$(getconf CLK_TCK)
python3 - "$out" "$label" "$engine" "$*" "${cmd[*]} -z ${dur}s" "$s0" "$s1" "$r0" "$r1" "$t0" "$t1" "$hz" <<'PY'
import json, sys
out, label, engine, args, cmd = sys.argv[1:6]
s0, s1, r0, r1, t0, t1, hz = map(float, sys.argv[6:13])
d = json.load(open(out + ".json")); el = t1 - t0
lat = sorted(x["latency"] for x in d["details"])
pct = lambda p: lat[min(len(lat) - 1, int(p / 100 * len(lat)))] / 1e6
ghz_cpu = sum(map(float, open(out + ".ghz_cpu").read().split())) / el * 100
line = (f"{label}: rps={d['rps']:.0f} p50={pct(50):.2f}ms p99={pct(99):.2f}ms p99.9={pct(99.9):.2f}ms "
        f"codes={d['statusCodeDistribution']} cpu%: server={(s1-s0)/hz/el*100:.0f} "
        f"redis(3 masters)={(r1-r0)/1e6/el*100:.0f} ghz={ghz_cpu:.0f}")
print(line)
with open(out + ".txt", "w") as f:
    f.write(line + "\nengine: " + engine + "\nscript args: " + args + "\nghz: " + cmd + "\n")
PY
gzip -f "$out.json"
