#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "redis/redis_backend.h"

struct redisContext;

namespace ratelimiter {

// Simplest working RedisBackend: one plain (non-cluster) Redis connection, owned by one worker
// thread that runs token_bucket.lua for each queued call in order. Callers never block; the
// callback runs on the worker thread. No pipelining or batching: correctness first, the batcher
// replaces this in Week 3.
class HiredisBackend final : public RedisBackend {
 public:
  // `lua_path` is the token_bucket.lua file. Throws std::runtime_error if it cannot be read.
  HiredisBackend(std::string host, int port, std::string lua_path);
  ~HiredisBackend() override;  // fails queued calls with redis_error, then joins the worker

  HiredisBackend(const HiredisBackend&) = delete;
  HiredisBackend& operator=(const HiredisBackend&) = delete;

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
            ScriptCallback done) override;

 private:
  struct Job {
    std::string redis_key;
    std::uint32_t capacity;
    double refill_per_sec;
    std::uint32_t requested;
    std::int64_t ttl_ms;
    ScriptCallback done;
  };

  struct ContextDeleter {
    void operator()(redisContext* c) const;
  };

  void Run();
  ScriptResult Execute(const Job& job);
  bool EnsureConnected();

  const std::string host_;
  const int port_;
  std::string script_;
  std::string sha_;

  // Touched only by the worker thread.
  std::unique_ptr<redisContext, ContextDeleter> ctx_;
  std::chrono::steady_clock::time_point next_connect_{};
  bool logged_down_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stop_{false};
  std::thread worker_;  // last member: starts after everything above is initialised
};

// Redis key of a token bucket: "{user:42}:api_default:tb". The hash tag is the limited
// entity, so every bucket of one user lands in the same Redis Cluster slot.
std::string BucketKey(std::string_view key, std::string_view rule);

}  // namespace ratelimiter
