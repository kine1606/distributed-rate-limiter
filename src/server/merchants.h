#pragma once
// Merchants (Phase 2, docs/PRODUCT_PLAN.md): the API owners who buy the service, each with its own
// customers. A call names its merchant by an API key in its `x-api-key` metadata; the merchant's
// customer `cust:42` is the key "<merchant>|cust:42" everywhere below the service, so merchants
// share no Redis key, plan, lease or denial. Keys are stored hashed (SHA-256, hex) in the hash
// {rl:merchants}:keys, field = hash, value = merchant id, written by tools/rlctl merchant-add.
// Every instance loads the whole hash at start and again on "!merchants" on plan-changes, with each
// merchant's limits over all its calls (P3, ratelimiter/types.h: MerchantLimits).

#include <openssl/evp.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

#include "ratelimiter/rule.h"
#include "ratelimiter/types.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

inline constexpr std::string_view kMerchantKeysHash = "{rl:merchants}:keys";
// P3: merchant -> its capacity, "rate R burst B reserve P" (set by the merchant), and merchant -> the
// checks a second the platform takes from it, "rate R burst B" (set by the operator).
inline constexpr std::string_view kMerchantCapacityHash = "{rl:merchants}:capacity";
inline constexpr std::string_view kMerchantCheckRateHash = "{rl:merchants}:check_rate";
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

// The terms of one of a merchant's buckets, as the management API writes them (control/rlcontrol/
// api.py): "rate 1000 burst 2000 reserve 20". A missing or malformed number is 0; a burst of 0 is one
// second of the rate; the reserve is a percentage, at most 100.
struct BucketTerms {
  std::uint32_t rate = 0, burst = 0, reserve = 0;
};
inline BucketTerms ParseBucketTerms(std::string_view text) {
  BucketTerms t;
  const auto word = [&text]() {
    const std::size_t from = text.find_first_not_of(' ');
    if (from == std::string_view::npos) return std::string_view{};
    const std::size_t to = text.find(' ', from);
    const std::string_view w = text.substr(from, to - from);
    text = to == std::string_view::npos ? std::string_view{} : text.substr(to);
    return w;
  };
  for (std::string_view name = word(), value = word(); !name.empty(); name = word(), value = word()) {
    std::uint32_t v = 0;
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
    if (ec != std::errc{} || end != value.data() + value.size()) v = 0;
    if (name == "rate") t.rate = v;
    if (name == "burst") t.burst = v;
    if (name == "reserve") t.reserve = std::min<std::uint32_t>(v, 100);
  }
  if (t.burst == 0) t.burst = t.rate;
  return t;
}

// A merchant as its calls see it. `limits.merchant` views `id`, so a Merchant never moves.
struct Merchant {
  std::string id;
  MerchantLimits limits;
};

class MerchantKeys {
 public:
  explicit MerchantKeys(RedisBackend& redis) : redis_(redis), id_(next_id_.fetch_add(1) + 1) {
    snapshot_.store(std::make_shared<const Snapshot>(), std::memory_order_release);
  }

  // Reads the keys, the merchants' capacities and their check rates again; `done(true)` once they are
  // in use, false if Redis did not answer (what was loaded before stays in use). Of reloads that
  // overlap, the one started last wins: an older one finishing late (a retry) never brings back a
  // revoked key.
  void Reload(std::function<void(bool)> done = {}) {
    const std::uint64_t ticket = tickets_.fetch_add(1, std::memory_order_relaxed) + 1;
    redis_.HGetAll(std::string(kMerchantKeysHash), [this, ticket, done = std::move(done)](const HashResult& keys) mutable {
      if (keys.redis_error) {
        if (done) done(false);
        return;
      }
      redis_.HGetAll(std::string(kMerchantCapacityHash), [this, ticket, keys, done = std::move(done)](const HashResult& capacity) mutable {
        if (capacity.redis_error) {
          if (done) done(false);
          return;
        }
        redis_.HGetAll(std::string(kMerchantCheckRateHash), [this, ticket, keys, capacity, done = std::move(done)](const HashResult& checks) {
          if (checks.redis_error) {
            if (done) done(false);
            return;
          }
          Publish(ticket, keys.fields, capacity.fields, checks.fields);
          if (done) done(true);
        });
      });
    });
  }
  bool ReloadAndWait(std::chrono::milliseconds timeout) {
    auto loaded = std::make_shared<std::promise<bool>>();
    auto f = loaded->get_future();
    Reload([loaded](bool ok) { loaded->set_value(ok); });
    return f.wait_for(timeout) == std::future_status::ready && f.get();
  }

  // The merchant `api_key` belongs to, or nullptr. Each thread keeps the keys it has resolved, with no
  // lock, and drops them all when new ones are published, so a revoked key stops at its next call.
  // Only a key this thread has not seen reads the shared table. The Merchant stays valid as long as
  // this MerchantKeys.
  const Merchant* Resolve(std::string_view api_key) {
    thread_local Cache cache;
    const std::uint64_t generation = generation_.load(std::memory_order_acquire);
    if (cache.owner != id_ || cache.generation != generation) {
      cache.keys.clear();
      cache.owner = id_;
      cache.generation = generation;
    }
    if (const auto it = cache.keys.find(api_key); it != cache.keys.end()) return it->second;
    const std::shared_ptr<const Snapshot> snapshot = snapshot_.load(std::memory_order_acquire);
    const auto it = snapshot->keys.find(Sha256Hex(api_key));
    if (it == snapshot->keys.end()) return nullptr;  // not cached: a stream of wrong keys cannot fill memory
    if (cache.keys.size() >= kCacheMax) cache.keys.clear();
    cache.keys.emplace(std::string(api_key), it->second);
    return it->second;
  }

  std::size_t Size() const { return snapshot_.load(std::memory_order_acquire)->keys.size(); }

 private:
  // One load: each key's merchant. Freed once replaced and no Resolve reads it any more.
  struct Snapshot {
    std::unordered_map<std::string, const Merchant*, StringHash, std::equal_to<>> keys;  // by key hash
  };
  struct Cache {
    std::uint64_t owner = 0;  // the MerchantKeys it belongs to (tests make several)
    std::uint64_t generation = 0;
    std::unordered_map<std::string, const Merchant*, StringHash, std::equal_to<>> keys;  // raw API key
  };
  static constexpr std::size_t kCacheMax = 4096;
  using Fields = std::unordered_map<std::string, std::string>;

  void Publish(std::uint64_t ticket, const Fields& keys, const Fields& capacity, const Fields& checks) {
    auto s = std::make_shared<Snapshot>();
    std::lock_guard lock(mu_);  // the registries and the ticket; reloads only, never the request path
    if (ticket < published_) return;  // a later reload already published newer keys
    published_ = ticket;
    std::unordered_map<std::string, const Merchant*> loaded;
    for (const auto& [hash, id] : keys) {
      if (!ValidMerchant(id)) continue;
      const Merchant*& m = loaded[id];
      if (m == nullptr) m = Limits(id, capacity, checks);
      s->keys.emplace(hash, m);
    }
    snapshot_.store(std::move(s), std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
  }

  // The merchant `id` with its limits, made from its terms. One per (id, limits), made the first time
  // and kept for good: a check still waiting holds its merchant's limits, and a reload must not free
  // them. So a reload with unchanged limits adds nothing; a change of limits adds one. Under mu_.
  const Merchant* Limits(const std::string& id, const Fields& capacity, const Fields& checks) {
    MerchantLimits l;
    if (const auto c = checks.find(id); c != checks.end()) {
      const BucketTerms t = ParseBucketTerms(c->second);
      l.checks = Bucket("merchant_checks", t.burst, t.rate);
    }
    if (const auto c = capacity.find(id); c != capacity.end()) {
      // The capacity, split: (100 - reserve)% of rate and burst for every plan, the rest only for
      // plans with a fee. A part with no rate or no burst is no bucket.
      const BucketTerms t = ParseBucketTerms(c->second);
      const std::uint32_t burst = static_cast<std::uint32_t>(std::uint64_t{t.burst} * (100 - t.reserve) / 100);
      const double rate = t.rate * (100.0 - t.reserve) / 100.0;
      l.capacity = Bucket("merchant_capacity", burst, rate);
      l.reserve = Bucket("merchant_reserve", t.burst - burst, t.rate - rate);
    }
    auto& m = merchants_[{id, l.checks, l.capacity, l.reserve}];
    if (!m) {
      m = std::make_unique<Merchant>(Merchant{id, l});
      m->limits.merchant = m->id;  // views its own copy, which never moves
    }
    return m.get();
  }

  // The token-bucket rule of these numbers: one per (name, burst, rate), made the first time and
  // kept for good, because leases refer to their rule; nullptr for a bucket that could never admit.
  // Merchants with the same numbers share it; their keys keep their buckets apart. Under mu_.
  // shortcut: never freed, so each new capacity a merchant sets adds a rule and a lease slot; a
  // merchant changing it ~10^6 times would fill the lease table. Limit changes per merchant in the API
  // if merchants are not trusted that far.
  const Rule* Bucket(const std::string& name, std::uint32_t burst, double rate) {
    if (burst == 0 || rate <= 0) return nullptr;
    auto& rule = rules_[{name, burst, rate}];
    if (!rule) {
      rule = std::make_unique<const Rule>(Rule{.name = name, .algorithm = Algorithm::kTokenBucket, .limit = burst,
                                               .refill_per_sec = rate, .failure_policy = FailurePolicy::kFailOpen});
    }
    return rule.get();
  }

  RedisBackend& redis_;
  const std::uint64_t id_;
  inline static std::atomic<std::uint64_t> next_id_{0};
  std::atomic<std::uint64_t> tickets_{0};
  std::mutex mu_;
  std::uint64_t published_ = 0;  // the ticket of the reload in use; under mu_
  std::map<std::tuple<std::string, std::uint32_t, double>, std::unique_ptr<const Rule>> rules_;
  std::map<std::tuple<std::string, const Rule*, const Rule*, const Rule*>, std::unique_ptr<Merchant>> merchants_;
  std::atomic<std::shared_ptr<const Snapshot>> snapshot_;
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
