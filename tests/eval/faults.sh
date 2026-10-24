#!/usr/bin/env bash
# Evaluation test 9, faults (docs/EVALUATION.md), on the development cluster; killing a master, which
# needs replicas, is bench/fault_kill_master.sh. One limiter (engine leased, defaults) and three
# open-loop streams, as in the kill test:
#   free   rule customer, cust:1..1000 (Free: fails closed)       1,000 checks/s
#   pay    rule customer, cust:pay:1..1000 (Pay: fails open)      5,000 checks/s
#   lease  rule api_default (token bucket served from leases)    10,000 checks/s
# Each fault is measured in three phases of 10 s, each with 1 s of warm-up: before, during, after.
#   latency20  +20 ms on every master's replies (below the 50 ms timeout)
#   latency80  +80 ms on one master's replies (above the timeout: its calls time out)
#   silent     one master's replies all lost (a partition without resets: nothing answers, nothing fails)
#   drops      every 2 s, CLIENT KILL of every normal connection on every master; once, the
#              plan-change subscription (CLIENT KILL TYPE pubsub)
# Latency comes from tc netem in the masters' shared network namespace (image rl-netem: alpine with
# iproute2, built on first use). FLUSHALL on the THROWAWAY dev cluster before every run.
#   tests/eval/faults.sh     env: RUNS=3  REDIS_PORT=7001  SCENARIOS="latency20 latency80 silent drops"
# Output: results/<date>_faults/summary.txt, one JSON line per scenario, run, phase and stream.
set -euo pipefail
cd "$(dirname "$0")/../.."
runs=${RUNS:-3} port=${REDIS_PORT:-7001}
out="results/$(date +%F)_faults"
mkdir -p "$out"
sum="$out/summary.txt"
echo "# $0 RUNS=$runs at commit $(git rev-parse --short HEAD); 1 instance, engine leased" >"$sum"

lport=30061 mport=30161  # below Linux's ephemeral range (see tests/eval/quota_accuracy.sh)
netns=ratelimiter-dev-redis-1-1  # the masters share this container's network namespace
docker image inspect rl-netem >/dev/null 2>&1 || printf 'FROM alpine:3.20\nRUN apk add --no-cache iproute2\n' | docker build -q -t rl-netem - >/dev/null
tc_ns() { docker run --rm --net "container:$netns" --cap-add NET_ADMIN rl-netem sh -c "$1"; }
clear_tc() { tc_ns "tc qdisc del dev eth0 root 2>/dev/null || true"; }
sp= dropper=
trap 'clear_tc; [ -n "$dropper" ] && kill "$dropper" 2>/dev/null; [ -n "$sp" ] && kill "$sp" 2>/dev/null; true' EXIT
masters() { redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}'; }
clear_tc

phase() {  # <scenario> <run> <phase>: the three streams at once, 1 s warm-up + 10 s
  local s=$1 r=$2 p=$3 pids=()
  for st in "free customer 1000 cust:" "pay customer 5000 cust:pay:" "lease api_default 10000 cust:"; do
    set -- $st
    build/release/bench/loadgen "127.0.0.1:$lport" "$2" 10 4 2000 1000 1 1 "$3" "$4" >"$out/${s}_r${r}_${p}_$1.json" &
    pids+=($!)
  done
  wait "${pids[@]}"
  for st in free pay lease; do
    echo "{\"scenario\":\"$s\",\"run\":$r,\"phase\":\"$p\",\"stream\":\"$st\",\"result\":$(cat "$out/${s}_r${r}_${p}_$st.json")}" >>"$sum"
  done
  curl -s "http://127.0.0.1:$mport/metrics" >"$out/${s}_r${r}_${p}.metrics.txt" || true
}

for r in $(seq "$runs"); do
  for s in ${SCENARIOS:-latency20 latency80 silent drops}; do
    for m in $(masters); do redis-cli -p "$m" FLUSHALL >/dev/null; done
    REDIS_PORT=$port tools/seed_plans.sh 1000 >/dev/null
    slow=$(redis-cli -p "$port" cluster nodes | awk '/master/ {split($2, a, "[:@]"); print a[2]}' | sort | sed -n 2p)
    RL_METRICS_ADDR="127.0.0.1:$mport" build/release/rate_limiter_server "127.0.0.1:$lport" config/rules.yaml \
      "127.0.0.1:$port" lua leased >"$out/${s}_r$r.server.log" 2>&1 &
    sp=$!
    for _ in $(seq 100); do (exec 3<>"/dev/tcp/127.0.0.1/$lport") 2>/dev/null && break; sleep 0.1; done
    phase "$s" "$r" before
    case $s in
      latency20) tc_ns "tc qdisc add dev eth0 root netem delay 20ms" ;;
      latency80) tc_ns "tc qdisc add dev eth0 root handle 1: prio && tc qdisc add dev eth0 parent 1:3 handle 30: netem delay 80ms &&
                        tc filter add dev eth0 protocol ip parent 1:0 prio 3 u32 match ip sport $slow 0xffff flowid 1:3" ;;
      silent) tc_ns "tc qdisc add dev eth0 root handle 1: prio && tc qdisc add dev eth0 parent 1:3 handle 30: netem loss 100% &&
                     tc filter add dev eth0 protocol ip parent 1:0 prio 3 u32 match ip sport $slow 0xffff flowid 1:3" ;;
      drops) (sleep 1.5; for i in 1 2 3 4 5; do
                for m in $(masters); do redis-cli -p "$m" CLIENT KILL TYPE normal >/dev/null; done
                [ "$i" = 3 ] && for m in $(masters); do redis-cli -p "$m" CLIENT KILL TYPE pubsub >/dev/null; done
                sleep 2; done) &
             dropper=$! ;;
    esac
    echo "$(date +%T.%3N) fault $s on (slow master: $slow)" >>"$out/${s}_r$r.server.log"
    phase "$s" "$r" during
    [ -n "$dropper" ] && { wait "$dropper" 2>/dev/null || true; dropper=; }
    clear_tc
    echo "$(date +%T.%3N) fault $s off" >>"$out/${s}_r$r.server.log"
    phase "$s" "$r" after
    kill "$sp"
    wait "$sp" 2>/dev/null || true
    sp=
    echo "run $r $s: done" >&2
  done
done
echo "summary: $sum"
