#include "plan/plan_store.h"

#include <algorithm>
#include <optional>
#include <utility>

#include "redis/keys.h"

namespace ratelimiter {

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
  Shard& s = ShardOf(customer);
  std::unique_lock lock(s.mu);
  const std::int64_t now = now_();
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
    lock.unlock();
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
        it->second.expires_ns = now_() + ttl_ns_;
        it->second.loading = false;
      } else {
        s.map.erase(it);  // an error is not cached: the next check asks Redis again
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
  if (it == s.map.end()) return;
  if (it->second.loading) {
    it->second.stale = true;
  } else {
    s.map.erase(it);
  }
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
}

}  // namespace ratelimiter
