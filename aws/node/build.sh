#!/usr/bin/env bash
# Builds the limiter and the load generator from ~/repo.tgz (uploaded by aws/kit.sh setup) with
# scripts/setup.sh's libraries, then packs what the other machines run into ~/bench.tgz.
set -euo pipefail
rm -rf ~/rl && mkdir ~/rl && tar -xzf ~/repo.tgz -C ~/rl && cd ~/rl
scripts/setup.sh
# Portable code (no -march=native): the limiter and load generator machines may be other CPU models.
# Tests are not built here: they ran on the laptop, and they would double the build time.
cmake --preset release -DRL_NATIVE=OFF -DRL_BUILD_TESTS=OFF >/dev/null
cmake --build build/release --target rate_limiter_server loadgen
mapfile -t libs < <(cd /usr/local/lib && ls libredis++.so*)  # redis-plus-plus, built by scripts/setup.sh
tar -czf ~/bench.tgz build/release/rate_limiter_server build/release/bench/loadgen config lua tools \
  -C /usr/local/lib "${libs[@]}"
echo "built: $(du -h ~/bench.tgz | cut -f1) to hand out"
