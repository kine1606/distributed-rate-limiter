#!/usr/bin/env bash
# End-to-end demo: curl -> gateway (HTTP :8080) -> rate_limiter_server (gRPC :50051) -> Redis Cluster
# (:7001-7003, docker/docker-compose.dev.yml). Starts the limiter and the gateway, sends a burst of
# requests, then stops what it started.  Usage: scripts/demo.sh [build preset, default release]
set -euo pipefail
cd "$(dirname "$0")/.."
preset="${1:-release}"
server="build/$preset/rate_limiter_server"

pids=()
tmp="$(mktemp -d)"
cleanup() {
  rm -rf "$tmp"
  for p in "${pids[@]}"; do kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
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

if ! redis-cli -p 7001 cluster info 2>/dev/null | grep -q cluster_state:ok; then
  echo "dev Redis Cluster not running: docker compose -f docker/docker-compose.dev.yml up -d" >&2
  exit 1
fi
masters="7001 7002 7003"
for m in $masters; do  # fresh buckets; a key found on a master lives there, so DEL needs no redirect
  redis-cli -p "$m" --scan --pattern '{user:*}:*' | sed 's/^/DEL /' | redis-cli -p "$m" >/dev/null
done

"$server" 127.0.0.1:50051 config/rules.yaml 127.0.0.1:7001 lua &
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
for m in $masters; do
  for k in $(redis-cli -p "$m" --scan --pattern '{user:*}:*'); do
    echo "$k  on :$m  $(redis-cli -p "$m" hgetall "$k" | paste -sd' ')"
  done
done
