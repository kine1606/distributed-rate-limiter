#include "core/leased_core.h"

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

namespace ratelimiter {
namespace {

// shortcut: fixed chunk of 10% of the limit; switch to the adaptive sizing in src/lease/README.md
// (step 5) once the benchmark shows refills on the critical path.
std::uint32_t Chunk(const Rule& rule) { return std::max<std::uint32_t>(1, rule.limit / 10); }

std::int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

CheckResult Allowed(const Rule& rule, std::int64_t left) {
  CheckResult r;
  r.allowed = true;
  r.limit = rule.limit;
  r.remaining = static_cast<std::uint32_t>(left);  // this instance's lease, not the global bucket
  return r;
}

CheckResult Denied(const Rule& rule, std::int64_t wait_ns) {
  CheckResult r;
  r.reason = Reason::kRateLimited;
  r.limit = rule.limit;
  r.retry_after_ms = static_cast<std::uint32_t>((wait_ns + 999'999) / 1'000'000);
  return r;
}

}  // namespace

LeasedCore::LeasedCore(RedisBackend& redis, PlanStore& plans, RuleTable rules, std::size_t lease_capacity)
    : redis_(redis), rules_(std::move(rules)), strict_(redis, plans, rules_), leases_(lease_capacity) {}

void LeasedCore::Check(const CheckRequest& request, CheckCallback done) {
  const auto it = rules_.find(request.rule);
  // shortcut: plan rules go strict, because a lease holds one rule's limit, not each customer's;
  // per-customer leases come with the plan cache (Week 4).
  if (it == rules_.end() || it->second.strict || it->second.limit_from != PlanLimit::kNone) {
    strict_.Check(request, std::move(done));
    return;
  }
  const Rule& rule = it->second;
  if (request.cost > rule.limit) {  // can never pass; parking it would only spin refills
    done(Denied(rule, 0));
    return;
  }
  Lease* lease = leases_.FindOrCreate(rule, request.key);
  if (lease == nullptr) {  // no room in the lease table around this key: exact path instead
    strict_.Check(request, std::move(done));
    return;
  }

  // Fast path: one CAS.
  if (const std::int64_t left = TryTake(*lease, request.cost); left >= 0) {
    // Refill before the lease runs dry, so under steady load nobody waits for Redis.
    if (left < Chunk(rule) * 3 / 10 && NowNs() >= lease->deny_until_ns.load(std::memory_order_relaxed)) {
      StartRefill(*lease, Chunk(rule));
    }
    done(Allowed(rule, left));
    return;
  }

  // Redis said no recently: answer locally until then instead of asking again.
  if (const std::int64_t wait = lease->deny_until_ns.load(std::memory_order_relaxed) - NowNs(); wait > 0) {
    done(Denied(rule, wait));
    return;
  }
  Park(*lease, request.cost, std::move(done));
}

void LeasedCore::Park(Lease& l, std::uint32_t cost, CheckCallback done) {
  std::int64_t left;
  {
    std::lock_guard lock(l.mu);
    left = TryTake(l, cost);  // a refill may have landed since the fast path looked
    if (left < 0) l.waiters.push_back({cost, std::move(done)});
  }
  if (left >= 0) {
    done(Allowed(l.rule, left));
    return;
  }
  StartRefill(l, std::max(Chunk(l.rule), cost));
}

void LeasedCore::StartRefill(Lease& l, std::uint32_t amount) {
  bool idle = false;
  if (!l.refill_in_flight.compare_exchange_strong(idle, true, std::memory_order_acq_rel)) return;
  Refill(l, amount, 0);
}

void LeasedCore::Refill(Lease& l, std::uint32_t amount, std::uint32_t partial_retry_ms) {
  redis_.Eval(l.rule, l.key, amount, [this, &l, amount, partial_retry_ms](const ScriptResult& s) {
    OnRefill(l, amount, partial_retry_ms, s);
  });
}

void LeasedCore::OnRefill(Lease& l, std::uint32_t requested, std::uint32_t partial_retry_ms,
                          const ScriptResult& s) {
  // Asked for more than Redis has left: take exactly what is left, and remember when the full
  // amount would have been there. Each retry asks for less, so this ends.
  if (!s.redis_error && !s.allowed && s.remaining > 0 && s.remaining < requested) {
    Refill(l, s.remaining, s.retry_after_ms);
    return;
  }
  // Redis is out (a denial, or a partial grant): whoever the lease cannot serve now is denied.
  const bool redis_out = !s.redis_error && (!s.allowed || partial_retry_ms > 0);
  const std::uint32_t retry_ms = s.allowed ? partial_retry_ms : s.retry_after_ms;

  std::vector<Waiter> waiters;
  {
    std::lock_guard lock(l.mu);
    if (s.allowed) l.tokens.fetch_add(requested, std::memory_order_relaxed);
    if (redis_out && retry_ms > 0) {
      l.deny_until_ns.store(NowNs() + std::int64_t{retry_ms} * 1'000'000, std::memory_order_relaxed);
    }
    l.refill_in_flight.store(false, std::memory_order_release);
    waiters.swap(l.waiters);
  }

  // Answer waiters in arrival order, outside the lock (callbacks finish gRPC calls).
  std::vector<Waiter> unserved;
  for (auto& w : waiters) {
    if (s.redis_error) {
      w.done(RedisDownResult(l.rule));
    } else if (const std::int64_t left = TryTake(l, w.cost); left >= 0) {
      w.done(Allowed(l.rule, left));
    } else if (redis_out) {
      w.done(Denied(l.rule, std::int64_t{retry_ms} * 1'000'000));
    } else {
      unserved.push_back(std::move(w));  // a full chunk came and went: Redis may have more
    }
  }
  if (unserved.empty()) return;

  std::uint32_t amount = Chunk(l.rule);
  {
    std::lock_guard lock(l.mu);
    for (auto& w : unserved) {
      amount = std::max(amount, w.cost);  // big waiters must not starve behind chunk-sized refills
      l.waiters.push_back(std::move(w));
    }
  }
  StartRefill(l, amount);  // no-op if another check already started one; it serves these too
}

}  // namespace ratelimiter
