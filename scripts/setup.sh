#!/usr/bin/env bash
# One-time setup of an Ubuntu 24.04 / WSL2 machine: libraries, tools and the development Redis.
# Usage: scripts/setup.sh   (asks for sudo; safe to run again). Docker Desktop must be running with
# WSL integration on for this distro (Settings > Resources > WSL integration).
set -euo pipefail
cd "$(dirname "$0")/.."

sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build pkg-config git curl \
  libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev libspdlog-dev libyaml-cpp-dev \
  libhiredis-dev libuv1-dev libgtest-dev libbenchmark-dev redis-tools python3-grpcio python3-grpc-tools

# redis-plus-plus: async Redis Cluster client (spike, Week 2 backend). Ubuntu does not package it.
if [ ! -f /usr/local/include/sw/redis++/async_redis_cluster.h ]; then
  src=$(mktemp -d)
  git clone -q --depth 1 -b 1.3.13 https://github.com/sewenew/redis-plus-plus.git "$src"
  cmake -S "$src" -B "$src/b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv \
    -DREDIS_PLUS_PLUS_CXX_STANDARD=20 -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
  cmake --build "$src/b"
  sudo cmake --install "$src/b"
  sudo ldconfig
  rm -rf "$src"
fi

# ghz (load tests) and grpcurl (manual calls): single static binaries in ~/.local/bin, which
# Ubuntu's ~/.profile puts on PATH at the next login.
mkdir -p ~/.local/bin
[ -x ~/.local/bin/ghz ] || curl -fsSL https://github.com/bojand/ghz/releases/download/v0.120.0/ghz-linux-x86_64.tar.gz |
  tar -xz -C ~/.local/bin ghz
[ -x ~/.local/bin/grpcurl ] || curl -fsSL https://github.com/fullstorydev/grpcurl/releases/download/v1.9.1/grpcurl_1.9.1_linux_x86_64.tar.gz |
  tar -xz -C ~/.local/bin grpcurl

# Development Redis: cluster on 127.0.0.1:7001-7003, throwaway plain Redis on 127.0.0.1:6380.
docker compose -f docker/docker-compose.dev.yml up -d
redis-cli -p 7001 cluster info | grep cluster_state
redis-cli -p 6380 ping

echo "setup done: now scripts/test.sh release"
