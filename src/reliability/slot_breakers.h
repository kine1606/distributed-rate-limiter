#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string_view>

#include "reliability/circuit_breaker.h"

namespace ratelimiter {

// CRC16 (XMODEM, as Redis Cluster uses) of `bytes`, modulo 16384.
inline std::uint16_t Crc16Slot(std::string_view bytes) {
  std::uint16_t crc = 0;
  for (const char c : bytes) {
    crc ^= static_cast<std::uint16_t>(static_cast<unsigned char>(c) << 8);
    for (int i = 0; i < 8; ++i) crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
  }
  return crc % 16384;
}

// The Redis Cluster hash slot of `key`: of its hash tag, the part between the first `{` and the
// next `}` when that part is not empty, else of the whole key.
inline std::uint16_t HashSlot(std::string_view key) {
  if (const auto open = key.find('{'); open != std::string_view::npos) {
    if (const auto close = key.find('}', open + 1); close != std::string_view::npos && close > open + 1) {
      return Crc16Slot(key.substr(open + 1, close - open - 1));
    }
  }
  return Crc16Slot(key);
}

// One circuit breaker per group of 256 hash slots. A whole-cluster breaker opened for every customer
// when one master died: the calls pending on its connections fail together, 20 in a row at once
// (results/2026-10-21_fault_kill_master/global_breaker). With groups, a dead master opens only the groups
// of its own slots; a group that straddles two masters is the only shared damage.
class SlotBreakers {
 public:
  static constexpr std::size_t kGroups = 64;

  explicit SlotBreakers(BreakerOptions options = {}, Clock clock = &SteadyNowNs) {
    for (std::size_t i = 0; i < kGroups; ++i) groups_.emplace_back(options, clock);
  }

  // The breaker of a Redis key (hash tag rules apply).
  CircuitBreaker& ForKey(std::string_view redis_key) { return Group(HashSlot(redis_key)); }
  // The breaker of the keys "{entity}:..." (src/redis/keys.h): Redis reads their tag as `entity` up
  // to its first `}`. An entity starting with `}` has an empty tag (the whole key decides): rare,
  // and any group is a correct breaker for it, only a less precise one.
  CircuitBreaker& ForEntity(std::string_view entity) { return Group(Crc16Slot(entity.substr(0, entity.find('}')))); }

  std::size_t Open() const {  // groups not closed: a metric, and readiness
    std::size_t n = 0;
    for (const auto& b : groups_) n += b.state() != CircuitBreaker::State::kClosed;
    return n;
  }

 private:
  CircuitBreaker& Group(std::uint16_t slot) { return groups_[slot * kGroups / 16384]; }

  std::deque<CircuitBreaker> groups_;  // a deque: breakers hold atomics and cannot move
};

}  // namespace ratelimiter
