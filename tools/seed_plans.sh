#!/usr/bin/env bash
# Seeds customer plans into the Redis Cluster (docs/CUSTOMER_PLANS.md, step B1). Overwrites the plan
# hashes it writes; touches nothing else.
#   tools/seed_plans.sh [count, default 10000]     env: REDIS_HOST=127.0.0.1 REDIS_PORT=7001 (any node)
# Writes {cust:1..count}:plan free, {cust:pay:1..count}:plan pay, {cust:ent:1..10}:plan enterprise.
set -euo pipefail
count=${1:-10000}
host=${REDIS_HOST:-127.0.0.1} port=${REDIS_PORT:-7001}

source "$(dirname "$0")/plans.sh"  # PLAN, the catalogue

cmds=$(mktemp)
trap 'rm -f "$cmds"' EXIT
seq "$count" | awk -v p="${PLAN[free]}" '{print "HSET {cust:" $1 "}:plan plan free " p}' >"$cmds"
seq "$count" | awk -v p="${PLAN[pay]}" '{print "HSET {cust:pay:" $1 "}:plan plan pay " p}' >>"$cmds"
seq 10 | awk -v p="${PLAN[enterprise]}" '{print "HSET {cust:ent:" $1 "}:plan plan enterprise " p}' >>"$cmds"

# --pipe is fast but does not follow MOVED, so every master gets every command: each one writes the
# keys of its own slots and answers MOVED for the rest. Those count as errors (non-zero exit, one
# line each on stderr), so the check below decides success instead.
masters=$(redis-cli -h "$host" -p "$port" cluster nodes | awk '/master/ && !/fail/ {split($2, a, "[:@]"); print a[1] ":" a[2]}')
[ -n "$masters" ] || { echo "no cluster masters found via $host:$port" >&2; exit 1; }
for m in $masters; do redis-cli -h "${m%:*}" -p "${m#*:}" --pipe <"$cmds" >/dev/null 2>&1 || true; done

for key in "{cust:$count}:plan" "{cust:pay:$count}:plan" "{cust:ent:10}:plan"; do
  [ -n "$(redis-cli -c -h "$host" -p "$port" hget "$key" daily)" ] || { echo "seeding failed: $key missing" >&2; exit 1; }
done
echo "seeded $((2 * count + 10)) plans on $(echo "$masters" | wc -l) masters"
