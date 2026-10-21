#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// What a customer bought, from the hash {cust:N}:plan (docs/CUSTOMER_PLANS.md). The defaults are the
// Free plan: a missing hash or field can never give away more than the cheapest plan.
struct Plan {
  std::uint32_t daily{200};
  std::uint32_t burst{2};
  std::uint32_t refill{2};  // tokens per second
  Overage overage{Overage::kHard};
  std::uint32_t extra{0};   // grace amount or overage cap
  bool suspended{false};    // plan "suspended" (step B9)
  FailurePolicy failure{FailurePolicy::kFailClosed};  // when Redis cannot be asked (Week 5)
};

// Identifies a plan's quota terms, so a denial recorded under them (DenyCache) is ignored once the
// plan changes. A collision would only keep a stale denial until the daily reset.
inline std::uint64_t QuotaStamp(const Plan& p) {
  return ((std::uint64_t{p.daily} << 32) | p.extra) ^ (static_cast<std::uint64_t>(p.overage) * 0x9e3779b97f4a7c15ULL);
}

// Parses HGETALL output. A missing or malformed field keeps its Free default, never an error: a
// broken plan written by an admin tool must not take a customer offline.
inline Plan ParsePlan(const std::unordered_map<std::string, std::string>& fields) {
  Plan p;
  const auto read = [&fields](const char* name, std::uint32_t& out) {
    const auto it = fields.find(name);
    if (it == fields.end()) return;
    std::uint32_t v;
    const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), v);
    if (ec == std::errc{} && end == it->second.data() + it->second.size()) out = v;
  };
  read("daily", p.daily);
  read("burst", p.burst);
  read("refill", p.refill);
  read("extra", p.extra);
  if (const auto it = fields.find("overage"); it != fields.end()) {
    if (it->second == "grace") p.overage = Overage::kGrace;
    if (it->second == "cap") p.overage = Overage::kCap;
  }
  if (const auto it = fields.find("plan"); it != fields.end()) p.suspended = it->second == "suspended";
  if (const auto it = fields.find("failure"); it != fields.end() && it->second == "open") {
    p.failure = FailurePolicy::kFailOpen;  // anything else keeps the safe default
  }
  return p;
}

}  // namespace ratelimiter
