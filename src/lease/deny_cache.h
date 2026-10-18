#pragma once

#include <algorithm>
#include <array>
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
// shortcut: one mutex per shard; a hot over-quota customer contends on one shard. Fine while the
// customer path reads the plan from Redis on every check; measure in the flood test (Week 6) before
// changing it.
class DenyCache {
 public:
  // Records that `customer` is out of quota for `reset_after_ms` after `now_ns` (any monotonic
  // clock, the same one passed to DeniedFor).
  void Deny(std::string_view customer, std::uint64_t plan_stamp, std::int64_t now_ns, std::uint32_t reset_after_ms) {
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    s.map.insert_or_assign(std::string(customer), Entry{now_ns + std::int64_t{reset_after_ms} * 1'000'000, plan_stamp});
    if (s.map.size() >= s.sweep_at) Sweep(s, now_ns);
  }

  // ms until the customer's quota resets, or 0 if no denial applies: none recorded, expired, or
  // recorded under another plan (then it is dropped).
  std::uint32_t DeniedFor(std::string_view customer, std::uint64_t plan_stamp, std::int64_t now_ns) {
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    const auto it = s.map.find(customer);
    if (it == s.map.end()) return 0;
    if (it->second.until_ns <= now_ns || it->second.plan_stamp != plan_stamp) {
      s.map.erase(it);
      return 0;
    }
    return static_cast<std::uint32_t>((it->second.until_ns - now_ns + 999'999) / 1'000'000);
  }

  void Clear(std::string_view customer) {  // a plan-change message (Week 4 plan cache)
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    if (const auto it = s.map.find(customer); it != s.map.end()) s.map.erase(it);
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
  struct Shard {
    std::mutex mu;
    std::unordered_map<std::string, Entry, StringHash, std::equal_to<>> map;
    std::size_t sweep_at = kMinSweep;
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
