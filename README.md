# Distributed Rate Limiter (C++20, gRPC, Redis, Lua)

A rate limiter service for microservices. Goal: at least 1M `Check` requests/s across the cluster,
p99 < 2 ms, using a hybrid design: a local token lease served with atomics, refilled in batches
from Redis through Lua scripts. See `docs/DESIGN.md` for the architecture and `docs/THESIS_PLAN.md`
for the schedule.

**Status: Week 1 done (proto v1 awaiting review), Week 3 started.** API contract v1 in
`proto/rate_limiter.proto`; Redis client chosen from a measured spike (`docs/DESIGN.md` section 3).
The default engine serves non-strict rules from a lock-free local lease
and refills it from Redis through pipelined Lua calls; `strict: true` rules ask Redis per check.
Token bucket, fixed window and sliding window scripts are implemented and tested against real
Redis. Measured on one laptop: ~31k checks/s, bound by the load generator (raw data and setup in
`results/2026-10-12_lease_engine/`, git-ignored). **Start with
[docs/ROADMAP.md](docs/ROADMAP.md)**: it is the week-by-week checklist, and every module folder has a
`README.md` with a skeleton, build order and hints.

## Layout

```text
proto/                  Check / CheckBatch API                         proto/README.md
include/ratelimiter/    Domain types, RateLimiterCore (no gRPC here)
src/server/             gRPC callback-API server                       src/server/README.md
src/core/               Engines: Strict, Leased, Composite             src/core/README.md
src/plan/               Customer plans, plan cache                     src/plan/README.md
src/lease/              Local lease fast path, deny cache              src/lease/README.md
src/redis/              Redis backend, script registry, batcher        src/redis/README.md
src/usage/              Usage events                                   src/usage/README.md
src/reliability/        Circuit breaker, retries, failure policy       src/reliability/README.md
src/metrics/            Prometheus metrics, logging rules              src/metrics/README.md
src/config/             YAML rules, SIGHUP reload                      src/config/README.md
lua/                    quota.lua, token_bucket.lua                    lua/README.md
config/rules.yaml       Rule templates
tools/                  seed_plans, rlctl, usage_report, plots         tools/README.md
tests/                  GoogleTest unit, script, integration, eval     tests/README.md
bench/                  Google Benchmark, load generator               bench/README.md
docker/                 Redis Cluster + limiter instances              docker/README.md
examples/               Spring Boot demo gateway (bonus)               examples/gateway-springboot/README.md
docs/                   ROADMAP, CUSTOMER_PLANS, DESIGN, EVALUATION, THESIS_PLAN, thesis/OUTLINE
results/                Raw benchmark output (git-ignored)             results/README.md
```

## Build and test

Develop on Linux (WSL2 Ubuntu 24.04 works) with Docker Desktop running and its WSL integration on.
One-time setup: Ubuntu packages, redis-plus-plus from source, `ghz` and `grpcurl`, and the
development Redis (cluster on 127.0.0.1:7001-7003, throwaway plain Redis on 127.0.0.1:6380):

```bash
scripts/setup.sh             # asks for sudo; safe to run again
docker compose -f docker/docker-compose.dev.yml up -d   # after a reboot: development Redis only

scripts/build.sh release     # -O3 -march=native
scripts/test.sh release
scripts/test.sh asan
scripts/test.sh tsan
```

Redis tests use 127.0.0.1:6380 (override with `RL_TEST_REDIS=host:port`) and skip when it is down.

`vcpkg.json` lists all dependencies (including the Redis client and prometheus-cpp needed later) if
you prefer vcpkg: `cmake --preset release -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.

## Run

```bash
# args: [listen] [rules.yaml] [redis host:port] [lua dir] [engine: leased | strict | allow_all]
# one plain Redis, not Cluster
./build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml 127.0.0.1:6380 lua leased
```

End-to-end demo (curl -> Python gateway on :8080 -> limiter -> Redis on :6380), starts Redis if needed:

```bash
scripts/demo.sh
```

Gateway alone: `python3 examples/gateway-python/gateway.py`, then
`curl -H 'X-User-Id: alice' localhost:8080/api/hello` (rule `demo`: 5 burst, 1/s; 429 after that).

`SIGINT`/`SIGTERM` marks health NOT_SERVING and drains in-flight requests for up to 5 seconds.

Docker environment (3 limiter instances on ports 50051-50053 sharing one Redis, plus the 3-master
cluster they move to with the Week 2 cluster backend):

```bash
docker compose -f docker/docker-compose.yml up --build
```

## Benchmarks

Micro: `./build/release/bench/bench_core`. One load-test cell: `bench/ghz_run.sh` (needs a throwaway
Redis on port 6380: it flushes it). Load test by hand: use [`ghz`](https://ghz.sh) against
`ratelimiter.v1.RateLimiter/Check` with `--proto proto/rate_limiter.proto`. Save raw output in
`results/` together with the command that produced it.

## Build stages

See [docs/ROADMAP.md](docs/ROADMAP.md) for the nine-week plan (Oct 12 to Dec 15, 2026).

Engineering rules for contributors and agents are in `AGENTS.md`.
