#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Customers known to be out of daily quota on this instance, so their next requests are denied
// without a Redis call (src/lease/README.md, step B4). An entry lasts until the reset time Redis
// returned, so it ends at the same moment as the Redis counter. Each entry also keeps a stamp of
// the plan it was made under: a check under a changed plan (an upgrade, step B5) ignores the entry,
// so a denial never outlives the plan that caused it, with or without a plan-change message.
//
// A lookup in a shard that holds no denial, the common case, is one atomic load and takes no lock.
// shortcut: a lookup in a shard with denials takes its mutex, so a flood from one over-quota
// customer contends on one shard (10 M lookups/s at 16 threads, results/2026-10-18_customer_microbench);
// make entries readable without the lock if the flood test (Week 6) shows it.
class DenyCache {
 public:
  // Records that `customer` is out of quota for `reset_after_ms` after `now_ns` (any monotonic
  // clock, the same one passed to DeniedFor).
  void Deny(std::string_view customer, std::uint64_t plan_stamp, std::int64_t now_ns, std::uint32_t reset_after_ms) {
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    s.map.insert_or_assign(std::string(customer), Entry{now_ns + std::int64_t{reset_after_ms} * 1'000'000, plan_stamp});
    if (s.map.size() >= s.sweep_at) Sweep(s, now_ns);
    s.size.store(s.map.size(), std::memory_order_relaxed);
  }

  // ms until the customer's quota resets, or 0 if no denial applies: none recorded, expired, or
  // recorded under another plan (then it is dropped).
  std::uint32_t DeniedFor(std::string_view customer, std::uint64_t plan_stamp, std::int64_t now_ns) {
    Shard& s = ShardOf(customer);
    // A Deny racing this load can be missed once; that check then asks Redis, which denies too.
    if (s.size.load(std::memory_order_relaxed) == 0) return 0;
    std::lock_guard lock(s.mu);
    const auto it = s.map.find(customer);
    if (it == s.map.end()) return 0;
    if (it->second.until_ns <= now_ns || it->second.plan_stamp != plan_stamp) {
      s.map.erase(it);
      s.size.store(s.map.size(), std::memory_order_relaxed);
      return 0;
    }
    return static_cast<std::uint32_t>((it->second.until_ns - now_ns + 999'999) / 1'000'000);
  }

  void Clear(std::string_view customer) {  // a plan-change message (Week 4 plan cache)
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    if (const auto it = s.map.find(customer); it != s.map.end()) s.map.erase(it);
    s.size.store(s.map.size(), std::memory_order_relaxed);
  }

  std::size_t Size() {
    std::size_t n = 0;
    for (Shard& s : shards_) {
      std::lock_guard lock(s.mu);
      n += s.map.size();
    }
    return n;
  }

 private:
  struct Entry {
    std::int64_t until_ns;
    std::uint64_t plan_stamp;
  };
  struct alignas(64) Shard {  // a lookup reads `size` of its shard only: no false sharing with others
    std::mutex mu;
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> map;
    std::size_t sweep_at = kMinSweep;
    std::atomic<std::size_t> size{0};  // map.size(), written under mu, read without it
  };
  static constexpr std::size_t kShards = 64;
  static constexpr std::size_t kMinSweep = 1024;

  Shard& ShardOf(std::string_view customer) { return shards_[StringHash{}(customer) % kShards]; }

  // Drops expired entries of customers who never came back. The threshold doubles with what is left,
  // so a shard full of live denials is swept O(1) times per insert on average.
  static void Sweep(Shard& s, std::int64_t now_ns) {
    std::erase_if(s.map, [now_ns](const auto& kv) { return kv.second.until_ns <= now_ns; });
    s.sweep_at = std::max(kMinSweep, 2 * s.map.size());
  }

  std::array<Shard, kShards> shards_;
};

}  // namespace ratelimiter
