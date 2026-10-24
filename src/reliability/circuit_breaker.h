#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

#include "ratelimiter/clock.h"

namespace ratelimiter {

struct BreakerOptions {
  std::uint32_t failure_threshold = 20;
  std::chrono::milliseconds open_for{500};
};

// Stops calling Redis while it keeps failing (src/reliability/README.md). Closed: every call goes
// out. `failure_threshold` failures in a row open it: calls fail at once, with no network call.
// After `open_for`, exactly one call goes out as a probe (half-open); its success closes the
// breaker, its failure opens it again for another `open_for`. Lock-free: atomics only.
//
// The count is of consecutive failures across everything one breaker covers. One for the whole
// cluster opened for every customer when a master died, because the calls pending on its
// connections fail together; ResilientBackend therefore uses one per slot group (SlotBreakers).
class CircuitBreaker {
 public:
  enum class State : std::uint8_t { kClosed, kHalfOpen, kOpen };  // the metric's 0, 1, 2

  explicit CircuitBreaker(BreakerOptions options = {}, Clock clock = &SteadyNowNs)
      : threshold_(options.failure_threshold),
        open_ns_(std::chrono::nanoseconds(options.open_for).count()),
        now_(clock) {}

  // Whether a call may go out now. While open, true once per `open_for`: that call is the probe.
  bool Allow() {
    const State s = state_.load(std::memory_order_acquire);
    if (s == State::kClosed) return true;
    if (s == State::kHalfOpen) return false;  // the probe is out
    if (now_() - opened_at_ns_.load(std::memory_order_relaxed) < open_ns_) return false;
    State expected = State::kOpen;
    return state_.compare_exchange_strong(expected, State::kHalfOpen, std::memory_order_acq_rel);
  }

  // Both return true when the breaker opened from closed, or closed again: the changes worth a log
  // line. A failed probe re-opens it silently (every `open_for` during an outage).
  // While open, a success is a late answer to a call sent before it opened: only the probe, in
  // half-open, may close it. When healthy this only reads (no shared write per call).
  bool OnSuccess() {
    if (failures_.load(std::memory_order_relaxed) != 0) failures_.store(0, std::memory_order_relaxed);
    State s = State::kHalfOpen;
    return state_.compare_exchange_strong(s, State::kClosed, std::memory_order_acq_rel);
  }

  bool OnFailure() {
    State s = state_.load(std::memory_order_acquire);
    if (s == State::kHalfOpen) {  // the probe failed: wait again
      opened_at_ns_.store(now_(), std::memory_order_relaxed);
      state_.compare_exchange_strong(s, State::kOpen, std::memory_order_acq_rel);
      return false;
    }
    if (failures_.fetch_add(1, std::memory_order_relaxed) + 1 < threshold_ || s != State::kClosed) return false;
    opened_at_ns_.store(now_(), std::memory_order_relaxed);
    return state_.compare_exchange_strong(s, State::kOpen, std::memory_order_acq_rel);
  }

  State state() const { return state_.load(std::memory_order_acquire); }

 private:
  const std::uint32_t threshold_;
  const std::int64_t open_ns_;
  const Clock now_;
  std::atomic<State> state_{State::kClosed};
  std::atomic<std::uint32_t> failures_{0};
  std::atomic<std::int64_t> opened_at_ns_{0};
};

}  // namespace ratelimiter
