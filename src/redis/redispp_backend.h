#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include <sw/redis++/async_redis++.h>

#include "redis/redis_backend.h"

namespace ratelimiter {

// RedisBackend on redis-plus-plus's AsyncRedisCluster (docs/DESIGN.md section 3). One libuv event
// loop thread drives one connection per master; calls issued without waiting share each
// connection's pipeline. Callbacks run on that thread, or inline when no connection is usable.
class RedisPPBackend final : public RedisBackend {
 public:
  // `host`:`port` is any cluster node; `lua_dir` holds the scripts (throws std::runtime_error if
  // one cannot be read). Waits up to 2 s for the cluster's slot map, then starts anyway: calls then
  // fail with redis_error until the cluster answers.
  RedisPPBackend(const std::string& host, int port, const std::string& lua_dir);

  RedisPPBackend(const RedisPPBackend&) = delete;
  RedisPPBackend& operator=(const RedisPPBackend&) = delete;

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, ScriptCallback done) override;
  void HGetAll(std::string key, HashCallback done) override;

 private:
  struct Script {
    std::string source;
    std::string sha;  // SHA1 of the source, computed locally: no SCRIPT LOAD needed to know it
  };
  struct Call;

  static std::array<Script, 4> LoadScripts(const std::string& lua_dir);
  void Send(const std::shared_ptr<Call>& call, bool with_source);
  void Fail(Call& call, std::string_view why);
  void Health(bool ok, std::string_view why);

  std::array<Script, 4> scripts_;  // indexed by Algorithm
  std::atomic<bool> down_{false};
  sw::redis::AsyncRedisCluster cluster_;  // last: destroyed first, so no callback outlives the rest
};

}  // namespace ratelimiter
