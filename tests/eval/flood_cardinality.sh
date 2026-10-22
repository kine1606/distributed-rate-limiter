#!/usr/bin/env bash
# Evaluation tests 5 and 6 (docs/EVALUATION.md). One limiter (engine leased, defaults, metrics on).
#   flood        a Free customer out of quota retries flat out (closed loop, 192 in flight), then at a
#                fixed 10,000 calls a second: Redis calls for that customer (INFO commandstats of its
#                master), deny-cache hits, and the latency of the denials
#   cardinality  1,000,000 Free customers (plus the 1,000,000 Pay and 10 enterprise plans the seed tool
#                writes): Redis memory per plan, then the customer rule and the leased api_default
#                over all of them against 10,000 of them: throughput, latency, Redis memory per
#                counter, limiter memory
# FLUSHALL on the THROWAWAY dev cluster.
#   tests/eval/flood_cardinality.sh     env: RUNS=3  REDIS_PORT=7001
# Output: results/<date>_flood_cardinality/summary.txt, one JSON line per measurement.
set -euo pipefail
cd "$(dirname "$0")/../.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_flood_cardinality"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); 1 instance, engine leased" >"$sum"

lport=30091 mport=30191  # below Linux's ephemeral range (see tests/eval/quota_accuracy.sh)
sp=
trap '[ -n "$sp" ] && kill "$sp" 2>/dev/null || true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }
flush() { for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done; }
memory() { local t=0 m; for m in $(masters); do t=$((t + $(redis-cli -p "$m" info memory | awk -F: '/^used_memory:/ {print $2}' | tr -d '\r'))); done; echo $t; }
keys() { local t=0 m; for m in $(masters); do t=$((t + $(redis-cli -p "$m" dbsize))); done; echo $t; }
calls() {  # script and plan-read calls answered by master port $1 so far
  redis-cli -p "$1" info commandstats | awk -F'[:=,]' '/^cmdstat_(evalsha|eval|hgetall):/ {n += $3} END {print n + 0}'
}
metric() { curl -s "http://127.0.0.1:$mport/metrics" | awk -v k="$1" '$1 == k {print $2}'; }
start() {
  RL_METRICS_ADDR="127.0.0.1:$mport" build/release/rate_limiter_server "127.0.0.1:$lport" config/rules.yaml \
    "127.0.0.1:$port" lua leased >"$out/server_$1.log" 2>&1 &
  sp=$!
  for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && return 0; sleep 0.1; done
  return 1
}
stop() { kill "$sp"; wait "$sp" 2>/dev/null || true; sp=; }
rss_kb() { awk '/^VmRSS/ {print $2}' "/proc/$sp/status"; }
load() { build/release/bench/loadgen "127.0.0.1:$lport" "$@"; }  # rule seconds channels in_flight keys warmup batch rate prefix

for r in $(seq "$runs"); do
  # ---- flood ----
  flush
  redis-cli -c -p "$port" HSET "{cust:flood:1}:plan" plan free daily 200 burst 100000 refill 100000 overage hard >/dev/null
  start "flood_r$r"
  load customer 1 1 1 1 0 1 0 cust:flood: >/dev/null  # uses up the 200 and puts the customer in the deny cache
  slot=$(redis-cli -p "$port" cluster keyslot cust:flood:1)
  owner=$(redis-cli -p "$port" cluster nodes | awk -v s="$slot" '/master/ { split($2, a, "[:@]");
    for (i = 9; i <= NF; i++) { n = split($i, x, "-"); if (s >= x[1] && s <= (n == 2 ? x[2] : x[1])) print a[2] } }')
  c0=$(calls "$owner") h0=$(metric rl_deny_cache_hits_total)
  closed=$(load customer 10 12 16 1 0 1 0 cust:flood:)
  c1=$(calls "$owner") h1=$(metric rl_deny_cache_hits_total)
  open=$(load customer 10 12 2000 1 0 1 10000 cust:flood:)
  c2=$(calls "$owner") h2=$(metric rl_deny_cache_hits_total)
  stop
  echo "{\"run\":$r,\"test\":\"flood\",\"redis_calls\":{\"closed\":$((c1 - c0)),\"open\":$((c2 - c1))},\"deny_hits\":{\"closed\":$((${h1%.*} - ${h0%.*})),\"open\":$((${h2%.*} - ${h1%.*}))},\"closed\":$closed,\"open\":$open}" \
    | tee -a "$sum" | cut -c1-200

  # ---- cardinality: 1,000,000 customers against 10,000 ----
  flush
  m0=$(memory) k0=$(keys)
  REDIS_PORT=$port tools/seed_plans.sh 1000000 >/dev/null
  m1=$(memory) k1=$(keys)
  echo "{\"run\":$r,\"test\":\"plans\",\"plans\":$((k1 - k0)),\"bytes\":$((m1 - m0)),\"bytes_per_plan\":$(((m1 - m0) / (k1 - k0)))}" | tee -a "$sum"
  for rule in customer api_default; do
    for n in 10000 1000000; do
      start "${rule}_${n}_r$r"
      ma=$(memory) ka=$(keys)
      res=$(load "$rule" 20 12 16 "$n" 2 1 0 cust:)
      mb=$(memory) kb=$(keys)
      echo "{\"run\":$r,\"test\":\"cardinality\",\"rule\":\"$rule\",\"customers\":$n,\"new_keys\":$((kb - ka)),\"new_bytes\":$((mb - ma)),\"limiter_rss_kb\":$(rss_kb),\"plan_reads\":$(metric rl_plan_cache_reads_total),\"lease_refills\":$(metric rl_lease_refills_total),\"load\":$res}" \
        | tee -a "$sum" | cut -c1-200
      stop
    done
  done
done
echo "summary: $sum"
