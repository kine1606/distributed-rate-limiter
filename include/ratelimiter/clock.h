#pragma once

#include <chrono>
#include <cstdint>

namespace ratelimiter {

// Nanoseconds of a monotonic clock. A function pointer, so tests can pass a fake clock at no cost
// to the hit path.
using Clock = std::int64_t (*)();

inline std::int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace ratelimiter
