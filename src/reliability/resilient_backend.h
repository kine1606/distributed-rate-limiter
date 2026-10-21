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

namespace ratelimiter {

struct RetryOptions {
  int max_retries = 2;
  std::chrono::microseconds base{1000};  // delay before retry n (from 0): base * 2^n * random(0.5, 1.5)
};

// A RedisBackend decorator (src/reliability/README.md): calls fail at once while the breaker is
// open, every answer feeds the breaker, and a timed-out call is sent again after a jittered delay,
// at most `max_retries` times. Only calls that cannot make a limit looser are retried, because a
// timed-out script may have run: plan reads, token-bucket and window scripts (a repeat takes one
// more token: stricter), and quota scripts with a request id (deduplicated by quota.lua). Quota
// scripts without one, and token returns, are never retried.
class ResilientBackend final : public RedisBackend {
 public:
  using StateObserver = std::function<void(CircuitBreaker::State)>;

  // `inner` and `breaker` must outlive this backend. `on_change` runs when the breaker opens from
  // closed or closes again (not on each failed probe), on the thread whose answer changed it.
  ResilientBackend(RedisBackend& inner, CircuitBreaker& breaker, RetryOptions retry = {},
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
  void Account(bool failed);  // feeds the breaker
  // Runs `retry(true)` after the n-th retry's delay, or `retry(false)` if stopped first.
  bool Schedule(int n, std::function<void(bool)> retry);
  void RunTimer(std::stop_token stop);

  RedisBackend& inner_;
  CircuitBreaker& breaker_;
  const RetryOptions retry_;
  const StateObserver on_change_;

  std::mutex mu_;  // retries only: they follow timeouts, so they are rare
  std::condition_variable_any wake_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> due_;
  bool stopped_ = false;
  std::jthread timer_;  // last: starts after the rest is set up
};

}  // namespace ratelimiter
