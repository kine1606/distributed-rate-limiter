# Chapter 4. Implementation

The design of Chapter 3 is implemented in C++20: 3,966 lines in `src/` and `include/`, 283 lines of
Lua for the Redis scripts, and 5,557 lines of tests. This chapter shows how the parts that carry the
thesis claims are built, with short excerpts and the measurements that decided them. The full code,
every script that produced a number, and the raw results are in the repository.

## 4.1 Project structure and build

The code is split into layers that depend only on the interfaces below them (`AGENTS.md`, rule 1).
The engines see Redis through one abstract class, `RedisBackend`, with three operations: run a
rule's script, give tokens back, and read a plan hash. A test can therefore put a fake Redis, a
transcription of the scripts on a fake clock, or the real cluster behind the same engine.

*Table 4.1: Source layout.*

| Directory | Contents |
| --- | --- |
| `include/ratelimiter/` | the public interface: `RateLimiterCore`, `Rule`, `CheckRequest`, `CheckResult`, the clock type |
| `src/core/` | the engines: `StrictCore` (one script per check), `LeasedCore` (leases), `CompositeCore` (the customer rule), `AllowAllCore` (no limiter, for measurement) |
| `src/lease/` | the lease table, the chunk policy and the deny cache |
| `src/plan/` | plans and the plan cache |
| `src/redis/` | the Redis client wrapper, sharding over clients, the batcher, the plan-change listener, key names |
| `src/reliability/` | the circuit breaker, breakers per slot group, the resilient backend (deadlines, retries) |
| `src/server/` | the gRPC service, request conversion, `main` |
| `src/metrics/`, `src/usage/`, `src/config/` | Prometheus text, usage events, the rules file |
| `lua/` | the four rule scripts and the token return |
| `tests/`, `bench/`, `tools/` | tests, load generators and benchmarks, admin and report tools |

The build uses CMake with five presets: `release` (`-O3 -march=native`), `debug`, `asan`, `tsan`
and `coverage`. `scripts/test.sh <preset>` builds a preset and runs every test. The libraries are
Ubuntu 24.04 packages except redis-plus-plus, which is built from source:

*Table 4.2: Libraries and their versions.*

| Library | Version | Used for |
| --- | --- | --- |
| gRPC, Protocol Buffers | 1.51.1, 3.21.12 | the service and its messages |
| redis-plus-plus over hiredis | built from source, hiredis 1.2.0 | the asynchronous Redis Cluster client |
| abseil | 20220623 (through gRPC) | gRPC's mutexes; one setting changed (4.5) |
| yaml-cpp, spdlog, OpenSSL | 0.8.0, 1.12.0, 3 | the rules file, logging, SHA1 of the scripts |
| GoogleTest, Google Benchmark | system packages | tests and microbenchmarks |

## 4.2 Lua scripts

Each rule's decision runs as one Lua script in Redis. Redis runs a script to completion before any
other command, so the read, the decision and the write are atomic, and two limiter instances can
never both take the last token (Chapter 2, 2.2.1). Three choices recur in every script.

- **Time from Redis.** A script reads `TIME` on the node that owns the key, so every instance agrees
  on the time and on the UTC day, and a limiter with a skewed clock cannot corrupt state. Elapsed
  time is clamped at zero, for a replica promoted with a clock behind.
- **Integers.** The token bucket stores thousandths of a token, and a refill adds elapsed
  milliseconds times the rate. Floating-point drift over millions of refills cannot arise.
- **A lifetime on every write.** An idle bucket expires once it would be full again, and a daily
  counter an hour after its day ends, so Redis memory follows the active customers.

*Listing 4.1: The decision of `lua/token_bucket.lua` (milli-tokens).*

```lua
tokens = math.min(cap_m, tokens + elapsed * rate)
local need = requested * 1000
if need <= tokens then
  tokens = tokens - need
  allowed = 1
elseif need <= cap_m and rate > 0 then
  retry_after = math.ceil((need - tokens) / rate)
end
```

`requested` is 1 for a direct check and a whole chunk for a lease refill, so one script serves both.

*Listing 4.2: Request ids and the overage policies in `lua/quota.lua`.*

```lua
if KEYS[2] then  -- a retry of a request already decided gets the stored answer
  local stored = redis.call('GET', KEYS[2])
  if stored then
    local a, u, r, o = string.match(stored, '^(%d+),(%d+),(%d+),(%d+)$')
    return { tonumber(a), tonumber(u), tonumber(r), reset_after, tonumber(o), 1 }
  end
end
-- ... a new UTC day starts the counters at zero ...
if used + cost <= limit then used = used + cost; allowed = 1
elseif policy == 'grace' then ...           -- up to limit + extra, free
elseif policy == 'cap' then ...             -- the part beyond the limit, billed, up to extra
end
redis.call('SET', KEYS[2], allowed .. ',' .. used .. ',' .. remaining .. ',' .. overage, 'PX', 300000)
```

The stored answer and the counter carry the same hash tag, so they live on one master and are
written by the same atomic script (3.6.2). The scripts are tested against a real Redis by 33 tests
(`tests/lua_scripts_test.cc`), including concurrency, clock jumps and day rollovers.

The client sends `EVALSHA` with a SHA1 computed locally (OpenSSL), so no `SCRIPT LOAD` round trip is
needed. A node that does not know the script, after a restart or a failover, answers `NOSCRIPT`, and
the call is sent once more as `EVAL` with the source.

## 4.3 Redis client and batching

**Client choice.** The backend uses redis-plus-plus's `AsyncRedisCluster`. An early spike reached
290,000 script calls a second from one thread on one connection per master, and about 360,000 on
three masters, where each master used about one core (`docs/DESIGN.md`, section 3). Pipelining came
free: calls issued without waiting leave together on the connection, and the pool size made no
difference. The library's cluster client offers callbacks for ordinary commands but only futures for
`EVALSHA`, so scripts go through `cluster.redis(key)`, a handle to the node that owns the key's slot,
which has callbacks. This handle does not follow a failover by itself. A script that meets `MOVED` or
a lost node is therefore sent once more after a routed command refreshes the slot map, except a quota
script after a lost connection, which may already have run (3.7.3).

**Several clients.** One redis-plus-plus client has one I/O thread, and that thread also runs every
callback, finishing the gRPC calls it answers. At 192 calls in flight on the exact path it was the
limit: 8,896 checks per second. `ShardedBackend` spreads keys over four clients by hash, so a key's
calls stay in order on one connection, and the exact path reached 36,283 checks per second
(`docs/thesis/evidence/reliability_2026-10-21.md`). Eight clients did worse than four on 12 hardware
threads.

**The batcher, measured and left off.** A batcher was built that collects calls and sends them
together, on a timer or adaptively. It gained nothing:

*Table 4.3: Batcher modes on the exact path (3 runs, `results/2026-10-18_batcher/NOTES.md`).*

| Mode | p50 at 1,000 checks/s | Checks/s at 192 in flight |
| --- | --- | --- |
| off | 0.450 ms | 11,625 |
| timer, 200 µs | 0.582 ms | 11,402 |
| adaptive | 0.438 ms | 11,147 |

The fixed timer cost 130 µs at the median, and neither mode raised throughput. Calls issued together
already leave in one write, and throughput under load was bound by round-trip time times concurrency,
not by writes. The batcher stays available (`RL_BATCHER`) for the ablation of Chapter 5.

## 4.4 The lease table

**Layout.** A lease is one cache line (`alignas(64)`), so two hot leases used from different cores
never share a line. The fields touched by every check come first and are atomics; the slow path's
fields follow.

*Listing 4.3: A lease (`src/lease/lease_table.h`).*

```cpp
struct alignas(64) Lease {
  std::atomic<std::int64_t> tokens{0};
  std::atomic<std::int64_t> deny_until_ns{0};  // Redis said no until then
  std::atomic<bool> refill_in_flight{false};   // single flight: one refill per lease at a time
  std::atomic<std::uint32_t> chunk;            // written only by the refill owner
  const std::uint64_t id;                      // 64-bit hash of (rule, key), compared first
  const Rule& rule;
  const std::string key;
  std::mutex mu;                               // slow path only: parked checks
  std::vector<Waiter> waiters;
};
```

The table is open addressing over a fixed array of atomic pointers. A slot goes from empty to a lease
exactly once, by compare-and-swap, and is never cleared or moved, so a lease pointer stays valid for
the table's life and lookups take no lock. A lookup compares the 64-bit id first and reads the key
string only on a match. The price is that leases are never evicted, a known limitation (6.2): one
instance used 224 MB for about 650,000 leases over 1,000,000 keys
(`flood_cardinality_2026-10-22.md`).

**The hit.** A check that finds tokens takes them with one compare-and-swap that never lets the count
go below zero.

*Listing 4.4: `TryTake`.*

```cpp
inline std::int64_t TryTake(Lease& l, std::uint32_t cost) {
  std::int64_t t = l.tokens.load(std::memory_order_relaxed);
  while (t >= cost) {
    if (l.tokens.compare_exchange_weak(t, t - cost, std::memory_order_relaxed)) return t - cost;
  }
  return -1;
}
```

Relaxed ordering suffices because the counter is the only data a hit reads; nothing else is published
through it. The tokens a refill adds are published by the refill path, under the lease's mutex, which
a check that found the lease empty takes too. A `fetch_sub` with an undo on failure was 1.8 to 2.7
times faster on one shared lease under contention (192 against 109 million operations a second on one
thread), but while the undo is pending another thread can be denied although tokens exist. The CAS
loop was kept. `alignas(64)` made per-thread counters 14 times faster at 16 threads (1,313 against
93.5 million operations a second; `results/2026-10-18_lease_microbench/NOTES.md`).

**Single flight.** A check that finds the lease empty parks under the mutex and calls `StartRefill`,
which sends a refill only if it wins the compare-and-swap of `refill_in_flight` (acquire-release, so
the winner sees the lease as the last refill left it). Five hundred concurrent misses on one key cause
one refill (tested). A refill asks for the chunk minus what the lease still holds (top-up, 3.5.2,
step 4), so an instance never holds more than one chunk, which the bound N × C requires. When the
refill lands, the parked checks are answered in arrival order outside the lock, because their
callbacks finish gRPC calls.

**The hot path stays free of locks, allocation and logging** (`CLAUDE.md`). Measured on the whole
engine, a hit over 10,000 keys runs at 23.9 million checks a second on one thread and 151.9 million on
16, with no allocation in the timed loop.

## 4.5 The gRPC server

**Callback API.** The service uses gRPC's callback API: a handler starts the engine and returns at
once, and whichever thread answers (a gRPC thread on a lease hit, a Redis I/O thread otherwise) calls
the reactor's `Finish`. No thread waits for Redis, and each call's `Finish` runs exactly once:
- for `Check`, from the engine's callback;
- for `CheckBatch`, from the callback of the last of its checks to answer. A shared state counts the
  checks still pending with an atomic counter, and each answer lands in its own response slot.

**At the trust boundary**, before any engine work, the service rejects with `INVALID_ARGUMENT`:
- an empty key or rule;
- a key or request id longer than 128 bytes;
- a batch of more than 1,000 checks.

A Redis outage is an answer (a reason in an OK response), never an RPC error (3.1.1).

**Backpressure** is one atomic counter of checks in flight. A call that would take it past 10,000 is
refused at once with `RESOURCE_EXHAUSTED`, and a batch larger than the limit itself is invalid, since
it could never be admitted. gRPC's own limit of concurrent calls per connection is set to 100
(`RL_MAX_STREAMS`). Without it, an open-loop load past capacity piled up thousands of calls per
connection and collapsed even with no limiter in the server (3.7.4).

**Engine swap and shutdown.** The service holds the engine through an atomic pointer.
- **Reload:** a rules reload builds a new engine and stores the pointer once (3.2.4).
- **Shutdown, in order:**
  1. set health to `NOT_SERVING`;
  2. drain calls for up to 5 s;
  3. return unused leased tokens;
  4. stop retries and answer attempts still waiting;
  5. stop the Redis clients while the engine is still alive, since an early refill may still be in flight.

**A one-line speed-up.** A profile of the server under load (`perf`, 2026-10-23) showed abseil's mutex
deadlock detection at work in gRPC's own threads: `GraphCycles::GetId` alone took 2.7% of the
CPU. Ubuntu's abseil checks every lock for cycles. `main` now switches the check off, which raised the
leased rule from 72,097 to 89,743 checks per second and the customer rule from 36,469 to 41,654 (medians
of 5 alternating runs; `verification_and_speedups_2026-10-23.md`).

## 4.6 Reliability

**Circuit breaker.** The breaker is a lock-free state machine: closed, open, half-open, held in one
atomic, with an atomic failure count and an atomic opening time. While closed, a success only reads
the count, so a healthy breaker writes no shared state per call. After 20 failures in a row it opens
for 500 ms. Then one compare-and-swap from open to half-open lets exactly one probe through, whose
answer closes the breaker or opens it again. A late success from before the opening cannot close it.
There is one breaker per group of 256 of Redis Cluster's 16,384 slots, computed from each key with
Redis's own CRC16 and hash-tag rule, so a dead master stops only its own customers' calls (3.7.2).

**Retries** are only for timeouts, and only for calls whose repetition cannot loosen a limit: plan
reads and token-bucket scripts, never quota scripts. They wait a jittered, doubling delay on one
timer thread and are counted against the breaker.

**Deadlines.** Each attempt is answered as timed out after 50 ms unless Redis answered first. The
client library's own timeout is an idle timeout per connection, re-armed by any reply, so it did not
bound a call: the fault test (5.7) found a slow master never timed out (3.7.2). Every Redis call
registers a deadline, so the structure that holds them is on a busy path, and a first version guarded
by a mutex cost 448 ns a call and serialised the threads. The final version is Vyukov's intrusive
multi-producer, single-consumer queue [26].

*Listing 4.5: Adding an attempt to the deadline queue: one atomic exchange.*

```cpp
void ResilientBackend::Push(Pending* p) {
  p->next.store(nullptr, std::memory_order_relaxed);
  Pending* prev = head_.exchange(p, std::memory_order_seq_cst);
  prev->next.store(p, std::memory_order_release);
}
```

- **The exchange** makes the attempt the new head. Linking the previous head to it publishes the
  attempt to the timer thread, which reads `next` with acquire.
- **Why the exchange is sequentially consistent.** When the timer has nothing to watch, it sets an
  `idle` flag and re-checks the queue before sleeping. A producer adds its attempt, then reads the
  flag. With both sides sequentially consistent, either the producer sees the flag and wakes the
  timer, or the timer's re-check sees the attempt; neither can miss the other.
- **Answering exactly once.** The attempt carries an atomic flag claimed by whichever comes first,
  the reply or the deadline.
- **Results.** The queue made a call 214 ns and gave 11 times the throughput at 12 threads; in the
  whole server the deadline's cost is within noise. A stress test of 24,000 attempts from 8 threads,
  answered at once, late or never, gets each answered exactly once, clean under TSan.

**Health and metrics.**
- **Readiness:** gRPC's health service reports `NOT_SERVING` while every slot group's breaker is open.
- **Metrics:** Prometheus text, written by the server itself because no client library was installed:
  - counters are striped over 16 cache lines, so adding to one is an uncontended atomic;
  - histograms have fixed bounds from 50 µs to 1 s;
  - a rule reload keeps every series and adds the new rules' series behind an atomic pointer to an
    immutable index.

The measured overhead of metrics was within noise.

## 4.7 Testing

*Table 4.4: Tests by kind (240 in all; every one passes in release, ASan and TSan).*

| Kind | Count | Against | Examples |
| --- | --- | --- | --- |
| Unit | 188 | fakes on a fake clock | each branch of the composite engine, lease protocol, plan cache races, breaker states, deadlines |
| Script | 33 | a real Redis (port 6380) | each algorithm's limits, request ids, overage, clock jumps, day rollover |
| Integration | 19 | the three-master cluster | exactly 200 of 400 concurrent calls; plan changes reaching two instances; leased quota on four instances; a killed subscription coming back |

Some tests do more than check examples:

- **A seeded harness for the bound** (3.5.4). One to four engines share a fake Redis that answers in a
  seeded random order, fails 2% of the calls, and runs on a fake clock with jumps. The theorem is
  checked over every window. It found two violations in the first protocol, and now passes 100,000 seeds.
- **Property tests against a reference model.**
  - **The histories:** random sequences of checks, retries, plan changes, Redis outages, clock jumps and
    UTC rollovers, run through the real engine on a transcription of the scripts.
  - **What must match:** every answer and every usage event must equal a separate model of Chapter 3's
    rules.
  - **The results:** 20,000 seeds per configuration produced 22.6 million matching checks. Ten bugs
    planted in the engine, one at a time, were all caught (`property_tests_2026-10-23.md`).
- **Sanitizers.** Every test runs under AddressSanitizer and ThreadSanitizer. TSan sometimes cannot
  start under the kernel's address randomisation, so the TSan suite runs with it off (`setarch -R`).
- **Coverage.** `scripts/coverage.sh` measures line and branch coverage of `src/` and `include/`: 97.3%
  of lines. The uncovered lines are defensive checks the scripts never trigger, and paths reachable
  only by a race between threads.
- **Evaluation scripts.** Each claim of Chapter 5 has a script in `tests/eval/` or `bench/`:
  - quota accuracy on four instances, 20 runs;
  - the burst bound, with a model;
  - flood and cardinality;
  - faults: latency, a silent master, dropped connections and a killed master;
  - the noisy neighbour;
  - the benchmark matrix.

  Each writes its raw output to `results/`, and the tables are generated from it
  (`tools/matrix_tables.py`, `tools/open_loop_table.py`; the ablation and A/B scripts print theirs).
