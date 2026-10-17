#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace ratelimiter {

// What a customer bought, from the hash {cust:N}:plan (docs/CUSTOMER_PLANS.md). The defaults are the
// Free plan: a missing hash or field can never give away more than the cheapest plan.
// overage, extra and failure are read once the engines use them (Weeks 4 and 5).
struct Plan {
  std::uint32_t daily{200};
  std::uint32_t burst{2};
  std::uint32_t refill{2};  // tokens per second
};

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
  return p;
}

}  // namespace ratelimiter
