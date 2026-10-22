#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
  // Each attempt that Redis has not answered within `deadline` is answered as timed out, and its late
  // reply is dropped; 0 leaves only the client's own timeout. That one is an idle timeout per
  // connection: a master answering every call 80 ms late under steady traffic never tripped it
  // (Week 6 fault test, docs/thesis/evidence/faults_2026-10-23.md).
  std::chrono::milliseconds deadline{0};
};

// A RedisBackend decorator (src/reliability/README.md): calls fail at once while the breaker is
// open, every answer feeds the breaker, and a timed-out call is sent again after a jittered delay,
// at most `max_retries` times. Each call is gated by the breaker of its key's slot group, so a dead
// master stops calls to its own slots only. Only calls that cannot make a limit looser are retried, because a
// timed-out script may have run: plan reads, token-bucket and window scripts (a repeat takes one
// more token: stricter). Quota scripts are never retried (a repeat counts twice, or with a request
// id comes back replayed and goes unbilled), nor are token returns. Only an unreachable or slow
// Redis feeds the breaker; an error reply (MOVED, a script error) does not. With a deadline, an
// attempt Redis has not answered in time counts as a timeout, whatever the connection does.
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

  // Answers retries still waiting with an error, and calls still waiting for a reply as timed out,
  // and sends no more retries. Call it before `inner` stops, so no retry reaches it afterwards;
  // answers arriving later are passed on without retrying.
  void StopRetries();

  // Attempts answered by their deadline because Redis had not answered (a metric; not on the hot path).
  std::uint64_t DeadlinesMissed() const { return deadlines_missed_.load(std::memory_order_relaxed); }

  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override;
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override;
  void HGetAll(std::string key, HashCallback done) override;

 private:
  struct ScriptCall;
  struct HashCall;
  struct Pending;  // one attempt: answered by its reply or by its deadline, whichever comes first

  // Registers an attempt's deadline: `expire` answers it as timed out unless the reply came first.
  // nullptr without a deadline, or once stopped: then only the reply answers.
  std::shared_ptr<Pending> Watch(std::function<void()> expire);
  static bool First(const std::shared_ptr<Pending>& p);  // claims the answer; false if already given

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

  std::mutex mu_;  // retries, and one deque append per attempt when a deadline is set
  std::condition_variable_any wake_;
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> due_;
  // Deadlines in send order: one fixed deadline makes this order their due order, so the timer only
  // looks at the front. It checks them in batches (kSweep), not once per attempt.
  std::deque<std::pair<std::chrono::steady_clock::time_point, std::shared_ptr<Pending>>> deadlines_;
  std::atomic<std::uint64_t> deadlines_missed_{0};
  bool stopped_ = false;
  std::jthread timer_;  // last: starts after the rest is set up
};

}  // namespace ratelimiter
