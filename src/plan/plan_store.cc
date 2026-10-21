#include "plan/plan_store.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "redis/keys.h"

namespace ratelimiter {
namespace {

// A thread's recent plan reads, direct-mapped by customer hash. Valid only for the store and the
// generation it was copied under, and until the cached plan's own expiry.
struct LocalPlan {
  std::uint64_t store = 0;
  std::uint64_t generation = 0;
  std::int64_t expires_ns = 0;
  std::string customer;
  Plan plan;
};
thread_local std::array<LocalPlan, 256> t_plans;

}  // namespace

void PlanStore::Read(std::string_view customer, Callback done) {
  redis_.HGetAll(PlanKey(customer), [done = std::move(done)](const HashResult& h) {
    if (h.redis_error) {
      done(nullptr);
      return;
    }
    const Plan plan = ParsePlan(h.fields);
    done(&plan);
  });
}

void PlanStore::Get(std::string_view customer, Callback done) {
  if (ttl_ns_ == 0) {
    Read(customer, std::move(done));
    return;
  }
  const std::size_t h = StringHash{}(customer);
  LocalPlan& local = t_plans[(h >> 8) % t_plans.size()];  // other bits than the shard index
  // Read before the shard: an invalidation after this load bumps it, so the copy made below dies.
  const std::uint64_t generation = generation_.load(std::memory_order_acquire);
  const std::int64_t now = now_();
  if (local.store == id_ && local.generation == generation && local.expires_ns > now && local.customer == customer) {
    const Plan plan = local.plan;  // `done` may call Get again and overwrite `local`
    done(&plan);
    return;
  }
  Shard& s = shards_[h % kShards];
  std::unique_lock lock(s.mu);
  auto it = s.map.find(customer);
  if (it == s.map.end()) {
    if (s.map.size() >= s.sweep_at) {  // drop the plans of customers who stopped calling
      std::erase_if(s.map, [now](const auto& kv) { return !kv.second.loading && kv.second.expires_ns <= now; });
      s.sweep_at = std::max(kMinSweep, 2 * s.map.size());
    }
    it = s.map.emplace(std::string(customer), Entry{}).first;
  } else if (it->second.loading) {
    it->second.waiters.push_back(std::move(done));
    return;
  } else if (it->second.expires_ns > now) {
    const Plan plan = it->second.plan;
    const std::int64_t expires = it->second.expires_ns;
    lock.unlock();
    local.store = id_;
    local.generation = generation;
    local.expires_ns = expires;
    local.customer.assign(customer);
    local.plan = plan;
    done(&plan);
    return;
  }
  it->second.loading = true;
  it->second.waiters.push_back(std::move(done));
  const std::string key = it->first;
  lock.unlock();
  Load(key);
}

void PlanStore::Load(const std::string& customer) {
  redis_.HGetAll(PlanKey(customer), [this, customer](const HashResult& h) { OnLoad(customer, h); });
}

void PlanStore::OnLoad(const std::string& customer, const HashResult& h) {
  std::optional<Plan> plan;
  if (!h.redis_error) plan = ParsePlan(h.fields);
  std::vector<Callback> waiters;
  bool again = false;
  {
    Shard& s = ShardOf(customer);
    std::lock_guard lock(s.mu);
    const auto it = s.map.find(customer);  // a loading entry is never erased
    if (it->second.stale) {
      it->second.stale = false;
      again = true;
    } else {
      waiters.swap(it->second.waiters);
      if (plan) {
        it->second.plan = *plan;
        it->second.has_plan = true;
        it->second.expires_ns = now_() + ttl_ns_;
        it->second.loading = false;
      } else if (it->second.has_plan) {  // stale if error; still expired, so the next check asks again
        plan = it->second.plan;
        it->second.loading = false;
      } else {
        s.map.erase(it);  // nothing to fall back on: the next check asks Redis again
      }
    }
  }
  if (again) {  // invalidated while loading: read again for everyone waiting
    Load(customer);
    return;
  }
  for (const Callback& w : waiters) w(plan ? &*plan : nullptr);
}

void PlanStore::Invalidate(std::string_view customer) {
  Shard& s = ShardOf(customer);
  std::lock_guard lock(s.mu);
  const auto it = s.map.find(customer);
  if (it != s.map.end()) {
    if (it->second.loading) {
      it->second.stale = true;
    } else {
      s.map.erase(it);
    }
  }
  generation_.fetch_add(1, std::memory_order_release);  // every thread's copies are stale now
}

void PlanStore::InvalidateAll() {
  for (Shard& s : shards_) {
    std::lock_guard lock(s.mu);
    for (auto it = s.map.begin(); it != s.map.end();) {
      if (it->second.loading) {
        it->second.stale = true;
        ++it;
      } else {
        it = s.map.erase(it);
      }
    }
  }
  generation_.fetch_add(1, std::memory_order_release);
}

}  // namespace ratelimiter
