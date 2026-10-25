#pragma once
// Merchants (Phase 2, docs/PRODUCT_PLAN.md): the API owners who buy the service, each with its own
// customers. A call names its merchant by an API key in its `x-api-key` metadata; the merchant's
// customer `cust:42` is the key "<merchant>|cust:42" everywhere below the service, so merchants
// share no Redis key, plan, lease or denial. Keys are stored hashed (SHA-256, hex) in the hash
// {rl:merchants}:keys, field = hash, value = merchant id, written by tools/rlctl merchant-add.
// Every instance loads the whole hash at start and again on "!merchants" on plan-changes.

#include <openssl/evp.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ratelimiter/rule.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

inline constexpr std::string_view kMerchantKeysHash = "{rl:merchants}:keys";
inline constexpr std::string_view kMerchantsChanged = "!merchants";  // control message on plan-changes
inline constexpr std::string_view kApiKeyHeader = "x-api-key";

// 1 to 64 of [A-Za-z0-9_-]: no '|', so "<merchant>|<customer>" names exactly one merchant's customer.
inline bool ValidMerchant(std::string_view m) {
  if (m.empty() || m.size() > 64) return false;
  for (const char c : m) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

inline std::string Sha256Hex(std::string_view s) {
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int n = 0;
  EVP_Digest(s.data(), s.size(), md, &n, EVP_sha256(), nullptr);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out(2 * n, '0');
  for (unsigned int i = 0; i < n; ++i) {
    out[2 * i] = kHex[md[i] >> 4];
    out[2 * i + 1] = kHex[md[i] & 15];
  }
  return out;
}

class MerchantKeys {
 public:
  using Index = std::unordered_map<std::string, std::string, StringHash, std::equal_to<>>;  // key hash -> merchant

  explicit MerchantKeys(RedisBackend& redis) : redis_(redis), id_(next_id_.fetch_add(1) + 1) { Publish(Index{}); }

  // Reads the hash again; `done(true)` once the new index is in use, false if Redis did not answer
  // (the previous index stays in use).
  void Reload(std::function<void(bool)> done = {}) {
    redis_.HGetAll(std::string(kMerchantKeysHash), [this, done = std::move(done)](const HashResult& h) {
      if (h.redis_error) {
        if (done) done(false);
        return;
      }
      Index index;
      for (const auto& [hash, merchant] : h.fields) {
        if (ValidMerchant(merchant)) index.emplace(hash, merchant);
      }
      Publish(std::move(index));
      if (done) done(true);
    });
  }
  bool ReloadAndWait(std::chrono::milliseconds timeout) {
    auto loaded = std::make_shared<std::promise<bool>>();
    auto f = loaded->get_future();
    Reload([loaded](bool ok) { loaded->set_value(ok); });
    return f.wait_for(timeout) == std::future_status::ready && f.get();
  }

  // The merchant `api_key` belongs to, or nullopt. Takes no lock: each thread keeps the keys it has
  // resolved, and drops them all when an index is published, so a revoked key stops at its next call.
  // The view stays valid until this thread's next Resolve.
  std::optional<std::string_view> Resolve(std::string_view api_key) {
    thread_local Cache cache;
    const std::uint64_t generation = generation_.load(std::memory_order_acquire);
    if (cache.owner != id_ || cache.generation != generation) {
      cache.keys.clear();
      cache.owner = id_;
      cache.generation = generation;
    }
    if (const auto it = cache.keys.find(api_key); it != cache.keys.end()) return std::string_view(it->second);
    const Index* index = index_.load(std::memory_order_acquire);
    const auto it = index->find(Sha256Hex(api_key));
    if (it == index->end()) return std::nullopt;  // not cached: a stream of wrong keys cannot fill memory
    if (cache.keys.size() >= kCacheMax) cache.keys.clear();
    return std::string_view(cache.keys.emplace(std::string(api_key), it->second).first->second);
  }

  std::size_t Size() const { return index_.load(std::memory_order_acquire)->size(); }

 private:
  struct Cache {
    std::uint64_t owner = 0;  // the MerchantKeys it belongs to (tests make several)
    std::uint64_t generation = 0;
    Index keys;  // raw API key -> merchant
  };
  static constexpr std::size_t kCacheMax = 4096;

  void Publish(Index index) {
    std::lock_guard lock(mu_);  // reloads only, never the request path
    indexes_.push_back(std::make_unique<const Index>(std::move(index)));
    index_.store(indexes_.back().get(), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  RedisBackend& redis_;
  const std::uint64_t id_;
  inline static std::atomic<std::uint64_t> next_id_{0};
  std::mutex mu_;
  // shortcut: every index published stays until exit, as a reader may still hold an old one; merchant
  // changes are rare. Free old ones by epoch if they become frequent.
  std::vector<std::unique_ptr<const Index>> indexes_;
  std::atomic<const Index*> index_{nullptr};
  std::atomic<std::uint64_t> generation_{0};
};

// "<merchant>|<key>", built in a buffer this thread reuses: no allocation once it has grown. The
// engines copy what they keep before Check returns (CheckRequest, include/ratelimiter/types.h).
inline std::string_view TenantKey(std::string_view merchant, std::string_view key) {
  thread_local std::string buffer;
  buffer.assign(merchant);
  buffer += '|';
  buffer.append(key);
  return buffer;
}

}  // namespace ratelimiter
