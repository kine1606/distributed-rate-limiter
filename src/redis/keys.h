#pragma once
// Every Redis key is built here, so the hash tag format cannot drift (docs/DESIGN.md section 4).
// The hash tag is the limited entity: all keys of one customer land in one Redis Cluster slot.

#include <array>
#include <string>
#include <string_view>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Key of one entity under one rule, e.g. "{cust:42}:daily_quota:q". The suffix names the algorithm,
// so a rule whose algorithm changes never reads state of the wrong shape.
inline std::string RedisKey(const Rule& rule, std::string_view key) {
  static constexpr std::array<std::string_view, 4> kSuffix{"tb", "sw", "fw", "q"};  // indexed by Algorithm
  return "{" + std::string(key) + "}:" + rule.name + ":" + std::string(kSuffix[static_cast<std::size_t>(rule.algorithm)]);
}

// A customer's plan hash, e.g. "{cust:42}:plan" (written by tools/seed_plans.sh, later rlctl).
inline std::string PlanKey(std::string_view customer) { return "{" + std::string(customer) + "}:plan"; }

}  // namespace ratelimiter
