#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ratelimiter {

// Values index per-algorithm tables (script files, key suffixes): append only.
enum class Algorithm { kTokenBucket, kSlidingWindow, kFixedWindow, kQuota };

// Where a rule's numbers come from: the rule itself, or the customer's plan in Redis
// ({cust:N}:plan). kBurst takes both the capacity (plan.burst) and the refill rate (plan.refill).
enum class PlanLimit : std::uint8_t { kNone, kDaily, kBurst };

// What to do when Redis is unreachable and the local lease is exhausted.
enum class FailurePolicy { kFailOpen, kFailClosed };

struct Rule {
  std::string name;
  Algorithm algorithm{Algorithm::kTokenBucket};
  std::uint32_t limit{0};            // burst capacity (token bucket) / max per window / daily quota
  double refill_per_sec{0.0};        // token bucket refill rate
  std::uint32_t window_ms{1000};     // window length (sliding and fixed window)
  bool strict{false};                // true: bypass leasing, exact Redis check per request
  PlanLimit limit_from{PlanLimit::kNone};  // not kNone: limit (and refill) come from the plan
  FailurePolicy failure_policy{FailurePolicy::kFailClosed};
};

// Transparent hash: a rule is found by the request's std::string_view without building a string.
struct StringHash {
  using is_transparent = void;
  std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

using RuleTable = std::unordered_map<std::string, Rule, StringHash, std::equal_to<>>;

}  // namespace ratelimiter
