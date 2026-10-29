#!/usr/bin/env bash
# What a machine needs to run the limiter or the load generator built on rl-loadgen-1 (the runtime
# libraries of docker/Dockerfile's last stage, and redis-cli), then ~/bench.tgz unpacked into ~/rl.
set -euo pipefail
if [ ! -f /usr/lib/x86_64-linux-gnu/libgrpc++.so.1.51 ]; then
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libgrpc++1.51t64 libprotobuf32t64 libspdlog1.12 \
    libyaml-cpp0.8 libhiredis1.1.0 libuv1t64 libssl3t64 redis-tools >/dev/null
fi
[ -f ~/bench.tgz ] || { echo "libraries installed; the build comes next"; exit 0; }
rm -rf ~/rl && mkdir ~/rl && tar -xzf ~/bench.tgz -C ~/rl
sudo mv ~/rl/libredis++.so* /usr/local/lib/ && sudo ldconfig
if ldd ~/rl/build/release/rate_limiter_server ~/rl/build/release/bench/loadgen | grep "not found" >&2; then
  exit 1  # a library the build machine had and this one does not
fi
echo "runtime ready"
