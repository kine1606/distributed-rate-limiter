#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "ratelimiter/rule.h"

namespace ratelimiter {

// Outcome of one Lua script call. Token bucket and windows return {allowed, remaining,
// retry_after_ms}; the quota script also returns reset_after_ms, which is then the retry time.
struct ScriptResult {
  bool allowed{false};
  std::uint32_t remaining{0};
  std::uint32_t retry_after_ms{0};
  bool redis_error{false};  // transport/timeout error, not a "denied" decision
  bool timed_out{false};    // the error was a timeout: the script may have run all the same
  bool reply_error{false};  // Redis answered with an error (MOVED, a script error): it is reachable
  std::uint32_t reset_after_ms{0};  // quota only: until 00:00 UTC
  bool overage{false};              // quota only: admitted as billed overage (cap policy)
  bool replayed{false};             // quota only: a retried request ID's stored answer, not counted again
  std::uint32_t limit{0};           // quota only: the limit in force, a boost included; 0 = not reported
};

using ScriptCallback = std::function<void(const ScriptResult&)>;

// A Redis hash (HGETALL); empty when the key does not exist.
struct HashResult {
  std::unordered_map<std::string, std::string> fields{};
  bool redis_error{false};
  bool timed_out{false};
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
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, ScriptCallback done) {
    Eval(rule, key, requested, {}, std::move(done));
  }

  // As above; for a quota rule a non-empty `request_id` (copied too) makes a retry with the same id
  // get the stored answer instead of being counted again. Other algorithms ignore it.
  virtual void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
                    ScriptCallback done) = 0;

  // Gives `tokens` back to the token bucket of `rule` and `key`, never above its capacity
  // (lua/return_lease.lua). Best effort; a backend that cannot do it answers redis_error.
  virtual void Return(const Rule&, std::string_view, std::uint32_t, ScriptCallback done) {
    ScriptResult r;
    r.redis_error = true;
    done(r);
  }

  virtual void HGetAll(std::string key, HashCallback done) = 0;
};

}  // namespace ratelimiter
