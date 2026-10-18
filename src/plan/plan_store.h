#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "plan/plan.h"
#include "ratelimiter/clock.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// A customer's plan, read from Redis. With a TTL the plan is cached (src/plan/README.md, Week 4):
// a hit answers inline with no Redis call; a miss sends one HGETALL per customer however many
// checks wait for it (single flight). Invalidate, called on a plan-change message, drops a plan
// sooner; the TTL bounds how stale a plan can be when a message was missed.
//
// A hit is lock-free: each thread keeps a small copy of the plans it read recently, valid while
// this store's generation is unchanged (Invalidate bumps it) and the TTL lasts. Only a thread's
// first read of a plan, or the first after a change, takes the shard mutex. Measured: one hot
// customer on 16 threads, results/2026-10-18_customer_microbench.
class PlanStore {
 public:
  using Callback = std::function<void(const Plan*)>;

  // `redis` must outlive this store. A zero `ttl` reads Redis on every call (the Week 2 baseline).
  explicit PlanStore(RedisBackend& redis, std::chrono::milliseconds ttl = {}, Clock clock = &SteadyNowNs)
      : redis_(redis), ttl_ns_(std::chrono::nanoseconds(ttl).count()), now_(clock), id_(NextId()) {}

  // `done` gets the plan (Free if the customer has none), or nullptr if Redis could not be asked.
  // It runs inline on a cache hit, else on a Redis I/O thread.
  void Get(std::string_view customer, Callback done);

  // The next Get reads Redis again. A read already in flight is discarded and sent again, since it
  // may have read the hash before the change was written.
  void Invalidate(std::string_view customer);
  void InvalidateAll();  // after the plan-change subscription was lost: messages may be missing

  std::size_t Size() {  // cached plans and reads in flight
    std::size_t n = 0;
    for (Shard& s : shards_) {
      std::lock_guard lock(s.mu);
      n += s.map.size();
    }
    return n;
  }

 private:
  struct Entry {
    Plan plan;
    std::int64_t expires_ns{0};
    bool loading{false};
    bool stale{false};               // invalidated while loading
    std::vector<Callback> waiters;   // while loading
  };
  struct Shard {
    std::mutex mu;
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> map;
    std::size_t sweep_at = kMinSweep;
  };
  static constexpr std::size_t kShards = 64;
  static constexpr std::size_t kMinSweep = 1024;

  Shard& ShardOf(std::string_view customer) { return shards_[StringHash{}(customer) % kShards]; }
  static std::uint64_t NextId() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }
  void Read(std::string_view customer, Callback done);  // no cache
  void Load(const std::string& customer);
  void OnLoad(const std::string& customer, const HashResult& h);

  RedisBackend& redis_;
  const std::int64_t ttl_ns_;
  const Clock now_;
  const std::uint64_t id_;  // tells this store's plans apart in the per-thread copies
  std::atomic<std::uint64_t> generation_{0};  // bumped after every invalidation
  std::array<Shard, kShards> shards_;
};

}  // namespace ratelimiter
