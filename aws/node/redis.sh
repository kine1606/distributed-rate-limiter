#!/usr/bin/env bash
# A Redis Cluster node for the benchmark, run by aws/kit.sh setup over SSH. Throwaway: no persistence,
# which the benchmarks require before they FLUSHALL. Port 6379 is open only inside the stack's security
# group. Restarting empties it, so `setup` can be run again.
set -euo pipefail
if ! command -v redis-server >/dev/null; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq redis-server >/dev/null
fi
sudo systemctl disable --now redis-server >/dev/null 2>&1 || true  # the packaged instance; ours is below
redis-cli -p 6379 shutdown nosave >/dev/null 2>&1 || true
mkdir -p ~/redis && cd ~/redis && rm -f nodes.conf
redis-server --port 6379 --bind 0.0.0.0 --protected-mode no --cluster-enabled yes --cluster-config-file nodes.conf \
  --cluster-node-timeout 5000 --appendonly no --save "" --daemonize yes --logfile "$HOME/redis/redis.log"
for _ in $(seq 50); do redis-cli -p 6379 ping >/dev/null 2>&1 && break; sleep 0.1; done
echo "redis $(redis-server --version | grep -o 'v=[0-9.]*') ready"
