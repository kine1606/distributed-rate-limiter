# Distributed Rate Limiter (C++20, gRPC, Redis, Lua)

A rate limiter service for microservices. Goal: at least 1M `Check` requests/s across the cluster,
p99 < 2 ms, using a hybrid design: a local token lease served with atomics, refilled in batches
from Redis through Lua scripts. See `docs/DESIGN.md` for the architecture and `docs/THESIS_PLAN.md`
for the schedule.

**Status: skeleton (Week 1 start).** The server runs end to end with an allow-all stub engine.
The Lua scripts are contracts only; the Redis client, lease manager and reliability layers are
built in the stages below.

## Layout

```text
proto/rate_limiter.proto      Check / CheckBatch API
include/ratelimiter/          Public domain types and the RateLimiterCore interface (no gRPC/protobuf here)
src/server/                   gRPC callback-API server (main.cc)
src/config/                   YAML rule loader
src/core/                     Engines (currently AllowAllCore stub)
src/redis/                    RedisBackend interface; concrete async cluster client goes here
src/lease/                    Local lease fast path (Week 3)
src/reliability/              Circuit breaker, backpressure, failure policy (Week 4)
src/metrics/                  Prometheus metrics (Week 4)
lua/                          token_bucket.lua, sliding_window.lua (contracts now, logic in Week 1)
config/rules.yaml             Example rules (hot-reload planned via SIGHUP)
tests/                        GoogleTest; tests/lua for script tests against real Redis
bench/                        Google Benchmark + load generator notes
docker/                       Dockerfile and compose: 3-master Redis Cluster + 3 limiter instances
scripts/                      build.sh, test.sh
docs/                         DESIGN.md, THESIS_PLAN.md
results/                      Raw benchmark output (git-ignored)
```

## Build and test

Develop on Linux (WSL2 Ubuntu works). Ubuntu 24.04 packages:

```bash
sudo apt-get install -y build-essential cmake ninja-build pkg-config libgrpc++-dev \
  protobuf-compiler-grpc libprotobuf-dev libspdlog-dev libyaml-cpp-dev libgtest-dev libbenchmark-dev

scripts/build.sh release     # -O3 -march=native
scripts/test.sh release
scripts/test.sh asan
scripts/test.sh tsan
```

`vcpkg.json` lists all dependencies (including the Redis client and prometheus-cpp needed later) if
you prefer vcpkg: `cmake --preset release -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.

## Run

```bash
./build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml
```

`SIGINT`/`SIGTERM` marks health NOT_SERVING and drains in-flight requests for up to 5 seconds.

Docker environment (3 Redis masters + 3 limiter instances on ports 50051-50053):

```bash
docker compose -f docker/docker-compose.yml up --build
```

## Benchmarks

Micro: `./build/release/bench/bench_core`. Load test: use [`ghz`](https://ghz.sh) against
`ratelimiter.v1.RateLimiter/Check` with `--proto proto/rate_limiter.proto`. Save raw output in
`results/` together with the command that produced it.

## Build stages

1. Lua + Redis correctness (scripts, cluster compose, script tests)
2. gRPC server with direct Redis calls (strict-mode baseline)
3. Local lease + batching
4. Reliability and observability
5. Benchmarks and the accuracy-bound test

Engineering rules for contributors and agents are in `AGENTS.md`.
