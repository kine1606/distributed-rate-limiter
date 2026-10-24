# Distributed Rate Limiter (C++20, gRPC, Redis, Lua)

A rate limiter service for microservices. Goal: at least 1M `Check` requests/s across the cluster,
p99 < 2 ms, using a hybrid design: a local token lease served with atomics, refilled in batches
from Redis through Lua scripts. See `docs/DESIGN.md` for the architecture and `docs/THESIS_PLAN.md`
for the schedule.

**Status: Weeks 1 to 7 done (2026-10-24).** Business rules (plans, daily quotas with request ids and
three overage policies, burst limits, a deny cache, a plan cache kept fresh over Pub/Sub, usage
events, `tools/rlctl`), leases with a proved bound, failure handling per plan, metrics, SIGHUP
reload, the evaluation, and Chapters 1 to 5 of the thesis in draft (`docs/thesis/`). The limiter runs
on a 3-master Redis Cluster through redis-plus-plus (`docs/DESIGN.md` section 3).

Measured on one laptop, where client, limiter and Redis share 12 hardware threads (evidence in
`docs/thesis/evidence/`, raw data in `results/`, git-ignored):

- **Quota exact:** 20 of 20 runs on four instances, duplicates counted once.
- **Leased rule, one instance:** 67,459 checks/s with single checks over Zipf keys, 95,913 on one hot
  key (within noise of gRPC with no limiter), 400,187 with `CheckBatch` of 10. p99 is 0.58 ms at 10,000 checks/s and
  1.45 ms at 20,000, open loop.
- **Burst over-admission:** at most N × C, measured within 0.3 tokens of a model.
- **Faults:** a slow, silent or killed master costs only its own customers, who get their plan's policy;
  no call failed.
- **Tests:** 240, passing in release, ASan and TSan; 97.3% line coverage.

1,000,000 checks/s is an estimate, not a measurement (`tools/extrapolate.py`): 15 to 50 instances,
depending on the latency target, and 17 Redis masters for customer checks. **Start with
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

Levels: `ctest --test-dir build/release -L unit` (no network), `-L redis` (Lua scripts on the plain
Redis at 127.0.0.1:6380, `RL_TEST_REDIS`), `-L integration` (backend and gRPC on the dev cluster at
127.0.0.1:7001, `RL_TEST_CLUSTER`). Redis-backed tests skip when their Redis is down.

Seed customer plans into the dev cluster (Free `cust:1..N`, Pay `cust:pay:1..N`, Enterprise
`cust:ent:1..10`): `tools/seed_plans.sh 10000`.

`vcpkg.json` lists all dependencies (including the Redis client and prometheus-cpp needed later) if
you prefer vcpkg: `cmake --preset release -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake`.

## Run

```bash
# args: [listen] [rules.yaml] [redis host:port] [lua dir] [engine: leased | strict | allow_all]
# redis = any node of the cluster (default 127.0.0.1:7001, the dev cluster)
./build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml 127.0.0.1:7001 lua leased
grpcurl -plaintext -d '{"key":"cust:42","rule":"daily_quota"}' localhost:50051 ratelimiter.v1.RateLimiter/Check
```

End-to-end demo (curl -> Python gateway on :8080 -> limiter -> dev Redis Cluster on :7001-7003):

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

Micro: `./build/release/bench/bench_core`. One load-test cell: `bench/ghz_run.sh` (runs FLUSHALL on
every master of the dev cluster, then seeds plans: never point it at a cluster holding real data). Load test by hand: use [`ghz`](https://ghz.sh) against
`ratelimiter.v1.RateLimiter/Check` with `--proto proto/rate_limiter.proto`. Save raw output in
`results/` together with the command that produced it.

## Build stages

See [docs/ROADMAP.md](docs/ROADMAP.md) for the nine-week plan (Oct 12 to Dec 15, 2026).

Engineering rules for contributors and agents are in `AGENTS.md`.
