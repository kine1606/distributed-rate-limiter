#!/usr/bin/env bash
# Evaluation test 11, noisy neighbour (docs/EVALUATION.md): what one flooding tenant does to the
# others. One limiter (engine leased, defaults). The others: Pay customers cust:pay:1..1000 on the
# customer rule, open loop at 5,000 checks/s, measured 10 s after 1 s of warm-up. The noisy tenant
# sends 20,000 calls a second (open loop, one key) from 1 s before the others start:
#   none        no noisy tenant (the baseline)
#   over_quota  a tenant out of daily quota: the deny cache answers it, no Redis call
#   over_burst  a tenant within quota but far over its burst (20 a second): each call runs the
#               burst script in Redis, which denies it
# Also recorded: the noisy tenant's Redis calls a second, from INFO commandstats of its master.
# FLUSHALL on the THROWAWAY dev cluster before every run.
#   tests/eval/noisy_neighbour.sh     env: RUNS=3  REDIS_PORT=7001
# Output: results/<date>_noisy_neighbour/summary.txt, one JSON line per scenario and run.
set -euo pipefail
cd "$(dirname "$0")/../.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_noisy_neighbour"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); 1 instance, engine leased" >"$sum"

lport=30081  # below Linux's ephemeral range (see tests/eval/quota_accuracy.sh)
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }
owner() {  # the master serving the key's hash slot
  local slot
  slot=$(redis-cli -p "$port" cluster keyslot "$1")
  redis-cli -p "$port" cluster nodes | awk -v s="$slot" '/master/ { split($2, a, "[:@]");
    for (i = 9; i <= NF; i++) { n = split($i, x, "-"); if (s >= x[1] && s <= (n == 2 ? x[2] : x[1])) print a[2] } }'
}
calls() { redis-cli -p "$1" info commandstats | awk -F'[:=,]' '/^cmdstat_(evalsha|eval|hgetall):/ {n += $3} END {print n + 0}'; }
load() { build/release/bench/loadgen "127.0.0.1:$lport" "$@"; }  # rule seconds channels in_flight keys warmup batch rate prefix

for r in $(seq "$runs"); do
  for s in none over_quota over_burst; do
    for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
    REDIS_PORT=$port tools/seed_plans.sh 1000 >/dev/null
    case $s in
      over_quota) redis-cli -c -p "$port" HSET "{cust:noisy:1}:plan" plan free daily 200 burst 100000 refill 100000 >/dev/null ;;
      over_burst) redis-cli -c -p "$port" HSET "{cust:noisy:1}:plan" plan pay daily 50000 burst 20 refill 20 >/dev/null ;;
    esac
    build/release/rate_limiter_server "127.0.0.1:$lport" config/rules.yaml "127.0.0.1:$port" lua leased \
      >"$out/${s}_r$r.server.log" 2>&1 &
    sp=$!
    for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
    [ "$s" = over_quota ] && load customer 1 1 1 1 0 1 0 cust:noisy: >/dev/null  # uses up its quota
    noisy_master=$(owner cust:noisy:1)
    c0=$(calls "$noisy_master")
    np=
    if [ "$s" != none ]; then
      load customer 13 4 4000 1 0 1 20000 cust:noisy: >"$out/${s}_r$r.noisy.json" &
      np=$!
      sleep 1
    fi
    load customer 10 4 2000 1000 1 1 5000 cust:pay: >"$out/${s}_r$r.others.json"
    c1=$(calls "$noisy_master")
    [ -n "$np" ] && wait "$np"
    kill "$sp"
    wait "$sp" 2>/dev/null || true
    sp=
    noisy=$([ -n "$np" ] && cat "$out/${s}_r$r.noisy.json" || echo null)
    echo "{\"scenario\":\"$s\",\"run\":$r,\"noisy_master_calls_per_s\":$(((c1 - c0) / 11)),\"others\":$(cat "$out/${s}_r$r.others.json"),\"noisy\":$noisy}" \
      | tee -a "$sum" | cut -c1-160
  done
done
echo "summary: $sum"
