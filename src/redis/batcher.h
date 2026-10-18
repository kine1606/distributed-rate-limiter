#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "redis/redis_backend.h"

namespace ratelimiter {

// Collects script calls and hands them to the inner backend in batches (src/redis/README.md,
// Week 3). kTimer sends when max_batch calls are queued or every max_delay. kAdaptive also sends a
// call at once when none is in flight, so a lone call never waits for the timer: calls queue only
// behind outstanding replies, and the queue goes out when the last reply returns (or the timer
// fires). Plan reads pass straight through. Off by default; RL_BATCHER=timer|adaptive turns it on.
class Batcher final : public RedisBackend {
 public:
  enum class Mode : std::uint8_t { kTimer, kAdaptive };

  // `inner` must outlive this batcher.
  Batcher(RedisBackend& inner, Mode mode, std::chrono::microseconds max_delay = std::chrono::microseconds(200),
          std::size_t max_batch = 128);
  ~Batcher() override;  // sends what is queued and waits for every reply, so no callback outlives it

  Batcher(const Batcher&) = delete;
  Batcher& operator=(const Batcher&) = delete;

  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override;
  // Rare (a quota denial after a burst token was taken), so not batched.
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    inner_.Return(rule, key, tokens, std::move(done));
  }
  void HGetAll(std::string key, HashCallback done) override { inner_.HGetAll(std::move(key), std::move(done)); }

 private:
  struct Call {
    Rule rule;  // copied: callers may pass a temporary (StrictCore's plan-filled rule)
    std::string key;
    std::uint32_t requested;
    std::string request_id;
    ScriptCallback done;
  };

  std::vector<Call> TakeQueueLocked();  // moves the queue out and counts it as in flight
  void Send(std::vector<Call>& batch);  // never called with mu_ held
  void OnReply(ScriptCallback& done, const ScriptResult& r);
  void RunTimer();

  RedisBackend& inner_;
  const Mode mode_;
  const std::chrono::microseconds max_delay_;
  const std::size_t max_batch_;

  std::mutex mu_;
  std::condition_variable timer_cv_, idle_cv_;
  std::vector<Call> queue_;
  std::size_t in_flight_ = 0;
  bool stop_ = false;
  std::thread timer_;  // started last, in the constructor body
};

}  // namespace ratelimiter
