#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "redis/redis_backend.h"
#include "reliability/circuit_breaker.h"
#include "reliability/slot_breakers.h"

namespace ratelimiter {

struct RetryOptions {
  int max_retries = 2;
  std::chrono::microseconds base{1000};  // delay before retry n (from 0): base * 2^n * random(0.5, 1.5)
};

// A RedisBackend decorator (src/reliability/README.md): calls fail at once while the breaker is
// open, every answer feeds the breaker, and a timed-out call is sent again after a jittered delay,
// at most `max_retries` times. Each call is gated by the breaker of its key's slot group, so a dead
// master stops calls to its own slots only. Only calls that cannot make a limit looser are retried, because a
// timed-out script may have run: plan reads, token-bucket and window scripts (a repeat takes one
// more token: stricter). Quota scripts are never retried (a repeat counts twice, or with a request
// id comes back replayed and goes unbilled), nor are token returns. Only an unreachable or slow
// Redis feeds the breaker; an error reply (MOVED, a script error) does not.
class ResilientBackend final : public RedisBackend {
 public:
  using StateObserver = std::function<void(std::size_t open_groups)>;

  // `inner` and `breakers` must outlive this backend. `on_change(open groups)` runs when a group
  // opens from closed or closes again (not on each failed probe), on the thread whose answer changed it.
  ResilientBackend(RedisBackend& inner, SlotBreakers& breakers, RetryOptions retry = {},
                   StateObserver on_change = {});
  ~ResilientBackend() override;

  ResilientBackend(const ResilientBackend&) = delete;
  ResilientBackend& operator=(const ResilientBackend&) = delete;

  // Answers retries still waiting with an error and sends no more. Call it before `inner` stops,
  // so no retry reaches it afterwards; answers arriving later are passed on without retrying.
  void StopRetries();

  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override;
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override;
  void HGetAll(std::string key, HashCallback done) override;

 private:
  struct ScriptCall;
  struct HashCall;

  void Send(const std::shared_ptr<ScriptCall>& c);
  void Send(const std::shared_ptr<HashCall>& c);
  void Account(CircuitBreaker& b, bool failed);  // feeds the breaker
  void Account(CircuitBreaker& b, const ScriptResult& r);
  // Runs `retry(true)` after the n-th retry's delay, or `retry(false)` if stopped first.
  bool Schedule(int n, std::function<void(bool)> retry);
  void RunTimer(std::stop_token stop);

  RedisBackend& inner_;
  SlotBreakers& breakers_;
  const RetryOptions retry_;
  const StateObserver on_change_;

  std::mutex mu_;  // retries only: they follow timeouts, so they are rare
  std::condition_variable_any wake_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> due_;
  bool stopped_ = false;
  std::jthread timer_;  // last: starts after the rest is set up
};

}  // namespace ratelimiter
