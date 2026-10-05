#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace ratelimiter {

enum class Algorithm { kTokenBucket, kSlidingWindow };

// What to do when Redis is unreachable and the local lease is exhausted.
enum class FailurePolicy { kFailOpen, kFailClosed };

struct Rule {
  std::string name;
  Algorithm algorithm{Algorithm::kTokenBucket};
  std::uint32_t limit{0};            // burst capacity (token bucket) / max per window (sliding window)
  double refill_per_sec{0.0};        // token bucket refill rate
  std::uint32_t window_ms{1000};     // sliding window length
  bool strict{false};                // true: bypass leasing, exact Redis check per request
  FailurePolicy failure_policy{FailurePolicy::kFailClosed};
};

using RuleTable = std::unordered_map<std::string, Rule>;

}  // namespace ratelimiter
