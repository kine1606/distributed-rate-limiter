#include "core/leased_core.h"

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ratelimiter {
namespace {

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

LeasedCore::LeasedCore(RedisBackend& redis, PlanStore& plans, RuleTable rules, std::size_t lease_capacity,
                       Clock clock, ChunkObserver on_chunk_change, LeaseTuning tuning)
    : redis_(redis),
      now_(clock),
      on_chunk_change_(std::move(on_chunk_change)),
      tuning_(tuning),
      rules_(std::move(rules)),
      strict_(redis, plans, rules_),
      leases_(lease_capacity) {}

void LeasedCore::Check(const CheckRequest& request, CheckCallback done) {
  const auto it = rules_.find(request.rule);
  // shortcut: plan rules go strict, because a lease holds one rule's limit, not each customer's;
  // per-customer leases come with the plan cache (Week 4).
  if (it == rules_.end() || it->second.strict || it->second.limit_from != PlanLimit::kNone) {
    strict_.Check(request, std::move(done));
    return;
  }
  CheckLease(it->second, request, std::move(done));
}

void LeasedCore::CheckLease(const Rule& rule, const CheckRequest& request, CheckCallback done) {
  if (request.cost > rule.limit) {  // can never pass; parking it would only spin refills
    done(Denied(rule, 0));
    return;
  }
  // A check bigger than one chunk would make this instance hold more than a chunk, breaking the
  // bound limit + N x C (the seeded harness found it). Rare, so it asks Redis directly.
  if (request.cost > ChunkCap(rule)) {
    strict_.Check(request, std::move(done));
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
    const std::uint32_t chunk = lease->chunk.load(std::memory_order_relaxed);
    if (tuning_.early_refill && left < std::int64_t{chunk} * 3 / 10 &&
        now_() >= lease->deny_until_ns.load(std::memory_order_relaxed)) {
      StartRefill(*lease, chunk);
    }
    done(Allowed(rule, left));
    return;
  }

  // Redis said no recently: answer locally until then instead of asking again.
  if (const std::int64_t wait = lease->deny_until_ns.load(std::memory_order_relaxed) - now_(); wait > 0) {
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
  if (left < 0) parked_.fetch_add(1, std::memory_order_relaxed);
  if (left >= 0) {
    done(Allowed(l.rule, left));
    return;
  }
  StartRefill(l, std::max(l.chunk.load(std::memory_order_relaxed), cost));
}

void LeasedCore::StartRefill(Lease& l, std::uint32_t target) {
  bool expected = false;
  if (!l.refill_in_flight.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
  // Top the lease up to `target` rather than adding `target` to what is left, so one instance never
  // holds more than one chunk (beyond what a parked waiter takes at once): the bound limit + N x C
  // counts exactly that. Adding a full chunk at the 30% watermark let an instance hold 1.3 x C, and
  // the seeded harness caught a window over N x C (tests/lease_harness_test.cc).
  const std::int64_t have = std::max<std::int64_t>(0, l.tokens.load(std::memory_order_relaxed));
  Refill(l, static_cast<std::uint32_t>(std::max<std::int64_t>(1, target - have)), 0);
}

void LeasedCore::Refill(Lease& l, std::uint32_t amount, std::uint32_t partial_retry_ms) {
  refills_.fetch_add(1, std::memory_order_relaxed);
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
  std::uint32_t chunk_from = 0, chunk_to = 0;
  {
    std::lock_guard lock(l.mu);
    if (s.allowed) l.tokens.fetch_add(requested, std::memory_order_relaxed);
    if (s.allowed && partial_retry_ms == 0) {  // only a full grant says something about demand
      const std::int64_t now = now_();
      const bool idle = l.last_refill_ns != 0 && now - l.last_refill_ns > kChunkIdleNs;
      chunk_from = l.chunk.load(std::memory_order_relaxed);
      chunk_to = tuning_.adaptive_chunk ? NextChunk(chunk_from, !l.waiters.empty(), idle, l.rule) : chunk_from;
      l.chunk.store(chunk_to, std::memory_order_relaxed);  // still the refill owner: no other writer
      l.last_refill_ns = now;
    }
    if (redis_out && retry_ms > 0) {
      l.deny_until_ns.store(now_() + std::int64_t{retry_ms} * 1'000'000, std::memory_order_relaxed);
    }
    l.refill_in_flight.store(false, std::memory_order_release);
    waiters.swap(l.waiters);
  }
  if (chunk_to != chunk_from && on_chunk_change_) on_chunk_change_(l, chunk_from, chunk_to);

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

  std::uint32_t amount = l.chunk.load(std::memory_order_relaxed);
  {
    std::lock_guard lock(l.mu);
    for (auto& w : unserved) {
      amount = std::max(amount, w.cost);  // big waiters must not starve behind chunk-sized refills
      l.waiters.push_back(std::move(w));
    }
  }
  StartRefill(l, amount);  // no-op if another check already started one; it serves these too
}

LeasedCore::Returned LeasedCore::ReturnLeases(std::chrono::milliseconds wait) {
  struct Pending {
    std::mutex mu;
    std::condition_variable cv;
    std::uint64_t left = 0, failed = 0;
  };
  auto pending = std::make_shared<Pending>();  // outlives a wait that times out
  Returned out{0, 0, 0, 0};
  const auto deadline = std::chrono::steady_clock::now() + wait;
  std::vector<Lease*> refilling;  // checks have stopped, so no new refill starts
  leases_.ForEach([&](Lease& l) {
    if (l.rule.algorithm == Algorithm::kTokenBucket && l.refill_in_flight.load(std::memory_order_acquire)) {
      refilling.push_back(&l);
    }
  });
  while (!refilling.empty() && std::chrono::steady_clock::now() < deadline) {
    std::erase_if(refilling, [](Lease* l) { return !l->refill_in_flight.load(std::memory_order_acquire); });
    if (!refilling.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  out.refills_unfinished = refilling.size();
  leases_.ForEach([&](Lease& l) {
    if (l.rule.algorithm != Algorithm::kTokenBucket) return;
    const std::int64_t tokens = l.tokens.exchange(0, std::memory_order_relaxed);
    if (tokens <= 0) return;
    ++out.leases;
    out.tokens += static_cast<std::uint64_t>(tokens);
    {
      std::lock_guard lock(pending->mu);
      ++pending->left;
    }
    redis_.Return(l.rule, l.key, static_cast<std::uint32_t>(tokens), [pending](const ScriptResult& r) {
      std::lock_guard lock(pending->mu);
      if (r.redis_error) ++pending->failed;
      if (--pending->left == 0) pending->cv.notify_all();
    });
  });
  std::unique_lock lock(pending->mu);
  pending->cv.wait_until(lock, std::max(deadline, std::chrono::steady_clock::now() + wait / 2),
                         [&] { return pending->left == 0; });
  out.failed = pending->failed + pending->left;
  return out;
}

}  // namespace ratelimiter
