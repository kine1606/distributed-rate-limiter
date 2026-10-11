#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Outcome of one Lua script call: {allowed, remaining, retry_after_ms}.
struct ScriptResult {
  bool allowed{false};
  std::uint32_t remaining{0};
  std::uint32_t retry_after_ms{0};
  bool redis_error{false};  // transport/timeout error, not a "denied" decision
};

using ScriptCallback = std::function<void(const ScriptResult&)>;

// Asynchronous, pipelined access to Redis Cluster. The lease manager and the strict path
// depend on this interface only, so they can be unit-tested with a fake backend.
// Implementations must not block the caller; the callback runs on a Redis I/O thread.
class RedisBackend {
 public:
  virtual ~RedisBackend() = default;

  // Runs the rule's Lua script atomically. `requested` is 1 for a direct strict check
  // and the chunk size for a lease refill (same script serves both).
  // `key` is copied before Eval returns, so the caller's buffer may die right after.
  virtual void Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                    ScriptCallback done) = 0;
};

}  // namespace ratelimiter
