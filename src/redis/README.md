# Redis client, backend and batcher

> Week 1 (spike), Week 2 (backend), Week 3 (batcher) · Flow step A9 · Status: interface only (`redis_backend.h`)

## What this module does

Everything that talks to Redis lives here, behind one interface. The engines never see a Redis
connection. They call `Eval(script, keys, args, callback)` and get a callback later on a Redis I/O
thread. That separation lets you unit-test engines with a fake backend, and lets you swap the
client library without touching the engines.

## Files to create

| File | Contains | Week |
| --- | --- | --- |
| `spike/spike_cluster_pipeline.cc` | Throwaway program that proves the client works | 1 |
| `redis_backend.h` | Interface (exists; generalise it as below) | 2 |
| `script_registry.{h,cc}` | Loads Lua files, keeps SHAs, reloads on `NOSCRIPT` | 2 |
| `redispp_backend.{h,cc}` | `RedisBackend` on redis-plus-plus `AsyncRedisCluster` | 2 |
| `fake_backend.h` | In-memory fake for engine unit tests | 2 |
| `batcher.{h,cc}` | Per-node queues, flush by size or time | 3 |

## Week 1: the client spike

The question to answer: can redis-plus-plus's async cluster client send many `EVALSHA` calls
without waiting for each reply, from several threads, over a small pool of connections? If yes, use
it. If not, fall back to hiredis with your own event loop. Spend at most two sessions on this.

Build it in this order:

0. Make the cluster reachable from WSL first (development profile on ports 7001 to 7003, see
   [docker/README.md](../../docker/README.md#week-1-task-reach-the-cluster-from-wsl-outside-docker)).
1. Install: `sudo apt install libhiredis-dev libuv1-dev`, then build redis-plus-plus with
   `-DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv -DREDIS_PLUS_PLUS_CXX_STANDARD=20`.
2. Write a 100-line program that connects with `sw::redis::AsyncRedisCluster`, loads
   `token_bucket.lua`, then fires 10,000 `EVALSHA` calls with futures or callbacks and waits for
   all of them at the end.
3. Measure calls per second for 1, 4 and 8 sending threads against the Docker cluster. Write the
   numbers down; they go into Chapter 4 as the reason for the choice.
4. Kill one master while it runs (`docker compose -f docker/docker-compose.yml stop redis-1`) and note what errors you see. This tells
   you what Week 5 must handle.

Skeleton:

```cpp
// spike_cluster_pipeline.cc  (not part of the main build)
#include <sw/redis++/async_redis_cluster.h>

int main() {
  sw::redis::ConnectionOptions opts;
  opts.host = "127.0.0.1"; opts.port = 7001;
  opts.socket_timeout = std::chrono::milliseconds(10);
  sw::redis::ConnectionPoolOptions pool;
  pool.size = 4;                                   // HINT: try 1, 4, 8 and compare
  sw::redis::AsyncRedisCluster cluster(opts, pool);

  // HINT: SCRIPT LOAD must run on every master. cluster.command() routes by key,
  //       so load it once per master through a plain AsyncRedis per node, or
  //       rely on the NOSCRIPT fallback (EVAL once, then EVALSHA).
  // HINT: build N futures of
  //       cluster.evalsha<std::vector<long long>>(sha, {key}, {cap, rate, req, ttl})
  //       then .get() them all; time the whole loop with steady_clock.
}
```

Write the decision into `docs/DESIGN.md` section "Redis client choice": what you tested, the
numbers, and why. The spec asks you to justify the choice, and a measured reason is the strongest
one.

## Week 2: the real backend

Generalise the interface first. Today it takes a `Rule`; it needs to run any of the scripts.

```cpp
// redis_backend.h
enum class Script : std::uint8_t { kTokenBucket, kQuota, kReturnLease };

enum class RedisStatus : std::uint8_t { kOk, kTimeout, kUnavailable, kScriptError };

struct ScriptCall {
  Script script;
  std::array<std::string, 2> keys;     // HINT: both share one hash tag
  std::uint8_t num_keys{1};
  std::array<std::int64_t, 6> args{};  // fixed size: no vector allocation per call
  std::uint8_t num_args{0};
};

struct ScriptReply {
  RedisStatus status{RedisStatus::kOk};
  std::array<std::int64_t, 5> values{}; // script return array, in order
  std::uint8_t count{0};
};

using ReplyCallback = std::function<void(const ScriptReply&)>;

class RedisBackend {
 public:
  virtual ~RedisBackend() = default;
  virtual void Eval(ScriptCall call, ReplyCallback done) = 0;  // never blocks
};
```

```cpp
// script_registry.h
class ScriptRegistry {
 public:
  explicit ScriptRegistry(std::string lua_dir);     // reads lua/*.lua into memory
  const std::string& Source(Script s) const;
  const std::string& Sha(Script s) const;           // SHA1 computed locally, see hint
};
```

Build it in this order:

1. `ScriptRegistry`: read each `.lua` file once at startup and compute its SHA1 locally (Redis uses
   the SHA1 of the script text), so you do not need `SCRIPT LOAD` to learn the SHA.
2. `RedisPPBackend::Eval`: call `evalsha`. On a `NOSCRIPT` error, call `eval` with the full source
   once (that also caches it on that node) and deliver that result. This is the fallback the spec
   requires, and it handles a failed-over master that never saw your `SCRIPT LOAD`.
3. Map client exceptions to `RedisStatus`: timeout to `kTimeout`, connection errors to
   `kUnavailable`. The engine decides what to do; the backend only reports.
4. `FakeBackend`: runs the same math in C++ with a fake clock. Engine tests use it so they run in
   milliseconds without Docker.
5. Integration test: start the backend against the Docker cluster and run 1,000 quota calls for
   100 customers; check every `used` value in Redis afterwards.

Hint: do not run the callback while holding any lock of yours. The engine's callback may call back
into the backend (a parked request starting a new refill), and that would deadlock.

## Week 3: the batcher

Without batching, each refill is one network round trip. The batcher collects calls for the same
Redis master and sends them in one write, so one round trip carries 50 to 200 decisions.

```cpp
// batcher.h
struct BatcherOptions {
  std::size_t max_batch = 128;                         // flush when this many are queued
  std::chrono::microseconds max_delay{200};            // or when the oldest waited this long
  std::size_t max_queue = 10'000;                      // bounded: reject beyond this
};

class Batcher final : public RedisBackend {
 public:
  Batcher(RedisBackend& inner, BatcherOptions opts);
  void Eval(ScriptCall call, ReplyCallback done) override;  // enqueue, return immediately
  // HINT: one queue per master, chosen by the key's hash slot (CRC16 of the hash tag % 16384)
  // HINT: one flusher thread, or a timer per queue, that drains a queue into a pipeline
  // HINT: metrics: batch size histogram, queue depth, flush reason (size vs time)
};
```

Build it in this order:

1. Start with a single queue for all keys and measure. Per-node queues only matter once you have
   more than one master in the benchmark.
2. Flush on size or on time, whichever comes first. Record which one triggered; if it is almost
   always time, your `max_delay` is too long for the load.
3. When the queue is full, call `done` immediately with `kUnavailable`. That becomes
   `RESOURCE_EXHAUSTED` higher up, which is the backpressure the spec asks for.
4. Benchmark: refills per second and p99 refill latency for `max_delay` of 100, 200 and 500 µs.
   This table goes into the tuning guide.

Hint: redis-plus-plus can pipeline per node for you if you issue many async calls without waiting.
Check in the spike whether that already gives you batching. If it does, the batcher becomes a thin
layer that only adds bounding and metrics, which is a perfectly good result to report.

## Done when

- [ ] Week 1: spike numbers written into `docs/DESIGN.md`; client chosen.
- [ ] Week 2: backend passes the integration test; `NOSCRIPT` fallback tested by running
      `SCRIPT FLUSH` mid-test.
- [ ] Week 3: batcher benchmark table saved in `results/`; TSan clean.
