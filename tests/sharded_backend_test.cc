// ShardedBackend (src/redis/sharded_backend.h): each key always reaches the same shard, so its calls
// stay in order on one connection; keys spread over every shard; each kind of call is passed on.
#include "redis/sharded_backend.h"

#include <gtest/gtest.h>

#include <memory>
#include <set>
#include <string>
#include <vector>

namespace ratelimiter {
namespace {

// Records what reaches it and answers at once.
class Recorder final : public RedisBackend {
 public:
  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view id, ScriptCallback done) override {
    calls.push_back("eval " + rule.name + " " + std::string(key) + " " + std::to_string(requested) + " " + std::string(id));
    done({.allowed = true, .remaining = requested});
  }
  void Return(const Rule&, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    calls.push_back("return " + std::string(key) + " " + std::to_string(tokens));
    done({.allowed = true});
  }
  void HGetAll(std::string key, HashCallback done) override {
    calls.push_back("hgetall " + key);
    done({.fields = {{"daily", "7"}}});
  }
  std::vector<std::string> calls;
};

struct Shards {
  Shards() {
    std::vector<std::unique_ptr<RedisBackend>> owned;
    for (int i = 0; i < 4; ++i) {
      auto r = std::make_unique<Recorder>();
      shard.push_back(r.get());
      owned.push_back(std::move(r));
    }
    backend = std::make_unique<ShardedBackend>(std::move(owned));
  }
  int Holder(const std::string& needle) {  // the one shard that saw it; -1 if none or several
    int found = -1;
    for (int i = 0; i < 4; ++i) {
      for (const auto& c : shard[static_cast<std::size_t>(i)]->calls) {
        if (c.find(needle) != std::string::npos) {
          if (found != -1 && found != i) return -1;
          found = i;
        }
      }
    }
    return found;
  }
  std::vector<Recorder*> shard;
  std::unique_ptr<ShardedBackend> backend;
};

const Rule kBucket{.name = "b", .limit = 10, .refill_per_sec = 1};

TEST(ShardedBackend, AKeyAlwaysUsesOneShardAndKeysSpreadOverEvery) {
  Shards s;
  std::set<int> used;
  for (int k = 0; k < 200; ++k) {
    const std::string key = "cust:" + std::to_string(k);
    for (int i = 0; i < 3; ++i) s.backend->Eval(kBucket, key, 1, [](const ScriptResult&) {});
    const int holder = s.Holder(" " + key + " ");
    ASSERT_NE(holder, -1) << key;  // all three calls of a key on one shard
    used.insert(holder);
  }
  EXPECT_EQ(used.size(), 4u);
}

TEST(ShardedBackend, EveryKindOfCallReachesTheKeysShardWithItsArguments) {
  Shards s;
  ScriptResult script;
  s.backend->Eval(kBucket, "cust:9", 5, "req-1", [&](const ScriptResult& r) { script = r; });
  EXPECT_EQ(script.remaining, 5u);
  const int shard = s.Holder("eval b cust:9 5 req-1");
  ASSERT_NE(shard, -1);
  s.backend->Return(kBucket, "cust:9", 3, [](const ScriptResult&) {});
  EXPECT_EQ(s.Holder("return cust:9 3"), shard);
  HashResult hash;
  s.backend->HGetAll("cust:9", [&](const HashResult& h) { hash = h; });
  EXPECT_EQ(s.Holder("hgetall cust:9"), shard);
  EXPECT_EQ(hash.fields.at("daily"), "7");
}

}  // namespace
}  // namespace ratelimiter
