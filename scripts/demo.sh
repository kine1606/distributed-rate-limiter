#!/usr/bin/env bash
# End-to-end demo: curl -> gateway (HTTP :8080) -> rate_limiter_server (gRPC :50051) -> Redis (:6379).
# Starts Redis if it is not already running, the limiter and the gateway, sends a burst of requests,
# then stops what it started.  Usage: scripts/demo.sh [build preset, default release]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
server="build/$preset/rate_limiter_server"

pids=()
started_redis=0
tmp="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp"
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  if [[ $started_redis == 1 ]]; then redis-cli -p 6379 shutdown nosave >/dev/null 2>&1 || true; fi
}
trap cleanup EXIT

wait_port() {  # wait_port <port> <name>
  for _ in $(seq 50); do
    (echo >"/dev/tcp/127.0.0.1/$1") 2>/dev/null && return 0
    sleep 0.1
  done
  echo "$2 did not open port $1" >&2
  exit 1
}

[[ -x $server ]] || scripts/build.sh "$preset"

if ! redis-cli -p 6379 ping >/dev/null 2>&1; then
  redis-server --port 6379 --save "" --appendonly no --daemonize yes >/dev/null
  started_redis=1
fi
wait_port 6379 redis
redis-cli -p 6379 --scan --pattern '{user:*}:*' | xargs -r redis-cli -p 6379 del >/dev/null  # fresh buckets

"$server" 127.0.0.1:50051 config/rules.yaml 127.0.0.1:6379 lua &
pids+=($!)
wait_port 50051 rate_limiter_server

python3 examples/gateway-python/gateway.py --listen 127.0.0.1:8080 --limiter 127.0.0.1:50051 2>/dev/null &
pids+=($!)
wait_port 8080 gateway

hit() {  # hit <user> <path>
  local code
  code=$(curl -s -o "$tmp/body" -D "$tmp/headers" -w '%{http_code}' -H "X-User-Id: $1" "http://127.0.0.1:8080$2")
  printf '%-6s %-11s -> %s  remaining=%-3s retry-after=%-2s %s\n' "$1" "$2" "$code" \
    "$(grep -i '^x-ratelimit-remaining' "$tmp/headers" | tr -dc 0-9)" \
    "$(grep -i '^retry-after' "$tmp/headers" | tr -dc 0-9)" "$(cat "$tmp/body")"
}

echo
echo "== alice: 7 requests to /api/hello (rule 'demo': burst 5, refill 1/s)"
for _ in $(seq 7); do hit alice /api/hello; done
echo "== bob gets a separate bucket"
hit bob /api/hello
echo "== alice waits 1.1 s: one token refilled"
sleep 1.1
hit alice /api/hello
hit alice /api/hello
echo "== what Redis holds"
for k in $(redis-cli -p 6379 --scan --pattern '{user:*}:*'); do
  echo "$k  $(redis-cli -p 6379 hgetall "$k" | paste -sd' ')"
done
