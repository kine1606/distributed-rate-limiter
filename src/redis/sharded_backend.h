#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// Spreads calls over several backends by key, so each key always uses the same one (its calls stay
// in order). With one RedisPPBackend per shard, each shard has its own I/O thread: one thread that
// also finishes every gRPC call it answers was the exact path's limit
// (docs/thesis/evidence/ceiling_and_envoy_2026-10-19.md).
class ShardedBackend final : public RedisBackend {
 public:
  explicit ShardedBackend(std::vector<std::unique_ptr<RedisBackend>> shards) : shards_(std::move(shards)) {}

  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override {
    Of(key).Eval(rule, key, requested, request_id, std::move(done));
  }
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    Of(key).Return(rule, key, tokens, std::move(done));
  }
  void HGetAll(std::string key, HashCallback done) override {
    RedisBackend& shard = Of(key);
    shard.HGetAll(std::move(key), std::move(done));
  }

 private:
  RedisBackend& Of(std::string_view key) { return *shards_[StringHash{}(key) % shards_.size()]; }

  std::vector<std::unique_ptr<RedisBackend>> shards_;
};

}  // namespace ratelimiter
