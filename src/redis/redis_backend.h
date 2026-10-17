#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Outcome of one Lua script call. Token bucket and windows return {allowed, remaining,
// retry_after_ms}; the quota script also returns reset_after_ms, which is then the retry time.
struct ScriptResult {
  bool allowed{false};
  std::uint32_t remaining{0};
  std::uint32_t retry_after_ms{0};
  bool redis_error{false};  // transport/timeout error, not a "denied" decision
  std::uint32_t reset_after_ms{0};  // quota only: until 00:00 UTC
};

using ScriptCallback = std::function<void(const ScriptResult&)>;

// A Redis hash (HGETALL); empty when the key does not exist.
struct HashResult {
  std::unordered_map<std::string, std::string> fields;
  bool redis_error{false};
};

using HashCallback = std::function<void(const HashResult&)>;

// Asynchronous, pipelined access to Redis Cluster. The engines and the plan store depend on this
// interface only, so they can be unit-tested with a fake backend.
// Implementations must not block the caller; callbacks run on a Redis I/O thread (or inline when
// Redis cannot be asked at all). Never hold a lock while calling them: a callback may call back
// into the backend.
class RedisBackend {
 public:
  virtual ~RedisBackend() = default;

  // Runs the rule's Lua script atomically. `requested` is 1 for a direct strict check
  // and the chunk size for a lease refill (same script serves both).
  // `rule` and `key` are copied before Eval returns, so both may die right after.
  virtual void Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                    ScriptCallback done) = 0;

  virtual void HGetAll(std::string key, HashCallback done) = 0;
};

}  // namespace ratelimiter
