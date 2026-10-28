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
  # A tag can be moved; the commit cannot: build only the code that was reviewed and tested.
  [ "$(git -C "$src" rev-parse HEAD)" = 3257aa87ca2591f0ede5d6293c47f47c6e0828bd ] ||
    { echo "redis-plus-plus 1.3.13 is not the commit expected" >&2; exit 1; }
  cmake -S "$src" -B "$src/b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv \
    -DREDIS_PLUS_PLUS_CXX_STANDARD=20 -DREDIS_PLUS_PLUS_BUILD_TEST=OFF
  cmake --build "$src/b"
  sudo cmake --install "$src/b"
  sudo ldconfig
  rm -rf "$src"
fi

# ghz (load tests) and grpcurl (manual calls): single static binaries in ~/.local/bin, which
# Ubuntu's ~/.profile puts on PATH at the next login.
# Each archive is checked against its SHA-256 (as each project publishes it) before it is unpacked.
mkdir -p ~/.local/bin
fetch() {  # fetch <url> <sha256> <file in the archive>
  local tgz
  tgz=$(mktemp)
  curl -fsSL "$1" -o "$tgz"
  echo "$2  $tgz" | sha256sum -c --quiet - || { echo "checksum mismatch: $1" >&2; rm -f "$tgz"; exit 1; }
  tar -xzf "$tgz" -C ~/.local/bin "$3"
  rm -f "$tgz"
}
[ -x ~/.local/bin/ghz ] || fetch https://github.com/bojand/ghz/releases/download/v0.120.0/ghz-linux-x86_64.tar.gz \
  c1b5eee89fd7c779213f198753d1d961dfbcb257bc470eb3c336f333060a6d6c ghz
[ -x ~/.local/bin/grpcurl ] || fetch https://github.com/fullstorydev/grpcurl/releases/download/v1.9.1/grpcurl_1.9.1_linux_x86_64.tar.gz \
  588c9c429476d9ed66cd3b2ae32283a6da36e0cfbb7e446f5d6a1b68dc770214 grpcurl

# Development Redis: cluster on 127.0.0.1:7001-7003, throwaway plain Redis on 127.0.0.1:6380.
docker compose -f docker/docker-compose.dev.yml up -d
redis-cli -p 7001 cluster info | grep cluster_state
redis-cli -p 6380 ping

echo "setup done: now scripts/test.sh release"
