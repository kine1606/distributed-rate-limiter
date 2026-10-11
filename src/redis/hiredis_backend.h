#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "redis/redis_backend.h"

struct redisContext;

namespace ratelimiter {

// Simplest working RedisBackend: one plain (non-cluster) Redis connection, owned by one worker
// thread that runs the rule's Lua script for each queued call in order. Callers never block; the
// callback runs on the worker thread. No pipelining or batching: correctness first, the batcher
// replaces this in Week 3.
class HiredisBackend final : public RedisBackend {
 public:
  // `lua_dir` holds token_bucket.lua, sliding_window.lua and fixed_window.lua.
  // Throws std::runtime_error if one cannot be read.
  HiredisBackend(std::string host, int port, const std::string& lua_dir);
  ~HiredisBackend() override;  // fails queued calls with redis_error, then joins the worker

  HiredisBackend(const HiredisBackend&) = delete;
  HiredisBackend& operator=(const HiredisBackend&) = delete;

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
            ScriptCallback done) override;

 private:
  struct Script {
    const char* file;
    std::string source;
    std::string sha;  // filled by SCRIPT LOAD on (re)connect
  };

  struct Job {
    std::string redis_key;
    Algorithm algorithm;
    std::vector<std::string> args;  // ARGV in the order the script expects
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
  std::array<Script, 3> scripts_;  // indexed by Algorithm

  // Touched only by the worker thread.
  std::unique_ptr<redisContext, ContextDeleter> ctx_;
  std::chrono::steady_clock::time_point next_connect_{};
  bool logged_down_{false};

  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Job> queue_;
  bool stop_{false};
  std::thread worker_;  // started at the end of the constructor, once the scripts are read
};

// Redis key of one entity under one rule, e.g. "{user:42}:api_default:tb". The hash tag is the
// limited entity, so all its keys land in one Redis Cluster slot. The suffix names the algorithm
// (tb, sw, fw), so a rule whose algorithm changes never reads state of the wrong shape.
std::string RedisKey(const Rule& rule, std::string_view key);

}  // namespace ratelimiter
