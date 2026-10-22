#include "reliability/resilient_backend.h"

#include <random>
#include <utility>
#include <vector>

namespace ratelimiter {

struct ResilientBackend::ScriptCall {
  Rule rule;  // copied: the caller's rule may be a temporary (StrictCore fills it from the plan)
  std::string key;
  std::uint32_t requested;
  std::string request_id;
  ScriptCallback done;
  CircuitBreaker* breaker;  // of the key's slot group
  int retries = 0;
};

struct ResilientBackend::Pending {
  std::atomic<bool> answered{false};
  std::function<void()> expire;
};

namespace {
const ScriptResult kScriptTimedOut{.redis_error = true, .timed_out = true};
const HashResult kHashTimedOut{.redis_error = true, .timed_out = true};
constexpr auto kSweep = std::chrono::milliseconds(5);  // how often deadlines are checked, at most
}  // namespace

struct ResilientBackend::HashCall {
  std::string key;
  HashCallback done;
  CircuitBreaker* breaker;
  int retries = 0;
};

ResilientBackend::ResilientBackend(RedisBackend& inner, SlotBreakers& breakers, RetryOptions retry,
                                   StateObserver on_change)
    : inner_(inner), breakers_(breakers), retry_(retry), on_change_(std::move(on_change)) {
  timer_ = std::jthread([this](std::stop_token stop) { RunTimer(stop); });
}

ResilientBackend::~ResilientBackend() { StopRetries(); }

void ResilientBackend::StopRetries() {
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> left;
  decltype(deadlines_) waiting;
  {
    std::lock_guard lock(mu_);
    if (stopped_) return;
    stopped_ = true;
    left.swap(due_);
    waiting.swap(deadlines_);
  }
  timer_.request_stop();
  if (timer_.joinable()) timer_.join();
  for (auto& [when, retry] : left) retry(false);
  for (auto& [when, p] : waiting) {
    if (First(p)) p->expire();
  }
}

std::shared_ptr<ResilientBackend::Pending> ResilientBackend::Watch(std::function<void()> expire) {
  if (retry_.deadline.count() == 0) return nullptr;
  auto p = std::make_shared<Pending>();
  p->expire = std::move(expire);
  bool first;
  {
    std::lock_guard lock(mu_);
    if (stopped_) return nullptr;
    first = deadlines_.empty();
    deadlines_.emplace_back(std::chrono::steady_clock::now() + retry_.deadline, p);
  }
  if (first) wake_.notify_one();  // otherwise the timer is already waiting for an earlier deadline
  return p;
}

bool ResilientBackend::First(const std::shared_ptr<Pending>& p) {
  return p == nullptr || !p->answered.exchange(true, std::memory_order_acq_rel);
}

void ResilientBackend::Account(CircuitBreaker& b, bool failed) {
  if ((failed ? b.OnFailure() : b.OnSuccess()) && on_change_) on_change_(breakers_.Open());
}

// An error reply (MOVED, a script error) means Redis answered: not a reason to stop calling it.
void ResilientBackend::Account(CircuitBreaker& b, const ScriptResult& r) { Account(b, r.redis_error && !r.reply_error); }

void ResilientBackend::Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                            std::string_view request_id, ScriptCallback done) {
  CircuitBreaker& breaker = breakers_.ForEntity(key);
  if (!breaker.Allow()) {
    done({.redis_error = true});
    return;
  }
  // A timed-out quota script may have counted the call; even with a request id, the retry would get
  // the stored answer marked replayed, and the admitted call would never be recorded for billing.
  if (rule.algorithm == Algorithm::kQuota || retry_.max_retries == 0) {
    auto answer = std::make_shared<ScriptCallback>([this, &breaker, done = std::move(done)](const ScriptResult& r) {
      Account(breaker, r);
      done(r);
    });
    auto pending = Watch([answer] { (*answer)(kScriptTimedOut); });
    inner_.Eval(rule, key, requested, request_id, [pending = std::move(pending), answer](const ScriptResult& r) {
      if (First(pending)) (*answer)(r);  // else the deadline answered: this reply is late
    });
    return;
  }
  Send(std::make_shared<ScriptCall>(
      ScriptCall{rule, std::string(key), requested, std::string(request_id), std::move(done), &breaker}));
}

void ResilientBackend::Send(const std::shared_ptr<ScriptCall>& c) {
  const auto answer = [this, c](const ScriptResult& r) {
    Account(*c->breaker, r);
    if (r.timed_out && c->retries < retry_.max_retries &&
        Schedule(c->retries++, [this, c](bool run) {
          if (run && c->breaker->Allow()) {
            Send(c);
          } else {
            c->done({.redis_error = true});
          }
        })) {
      return;
    }
    c->done(r);
  };
  auto pending = Watch([answer] { answer(kScriptTimedOut); });
  inner_.Eval(c->rule, c->key, c->requested, c->request_id, [pending = std::move(pending), answer](const ScriptResult& r) {
    if (First(pending)) answer(r);
  });
}

void ResilientBackend::Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) {
  CircuitBreaker& breaker = breakers_.ForEntity(key);
  if (!breaker.Allow()) {
    done({.redis_error = true});
    return;
  }
  auto answer = std::make_shared<ScriptCallback>([this, &breaker, done = std::move(done)](const ScriptResult& r) {
    Account(breaker, r);
    done(r);
  });
  auto pending = Watch([answer] { (*answer)(kScriptTimedOut); });
  inner_.Return(rule, key, tokens, [pending = std::move(pending), answer](const ScriptResult& r) {
    if (First(pending)) (*answer)(r);
  });
}

void ResilientBackend::HGetAll(std::string key, HashCallback done) {
  CircuitBreaker& breaker = breakers_.ForKey(key);
  if (!breaker.Allow()) {
    done({.redis_error = true});
    return;
  }
  Send(std::make_shared<HashCall>(HashCall{std::move(key), std::move(done), &breaker}));
}

void ResilientBackend::Send(const std::shared_ptr<HashCall>& c) {
  const auto answer = [this, c](const HashResult& h) {
    Account(*c->breaker, h.redis_error);  // plan reads are routed: redirects are followed, errors mean unreachable
    if (h.timed_out && c->retries < retry_.max_retries &&
        Schedule(c->retries++, [this, c](bool run) {
          if (run && c->breaker->Allow()) {
            Send(c);
          } else {
            c->done({.redis_error = true});
          }
        })) {
      return;
    }
    c->done(h);
  };
  auto pending = Watch([answer] { answer(kHashTimedOut); });
  inner_.HGetAll(c->key, [pending = std::move(pending), answer](const HashResult& h) {
    if (First(pending)) answer(h);
  });
}

bool ResilientBackend::Schedule(int n, std::function<void(bool)> retry) {
  thread_local std::mt19937_64 rng{std::random_device{}()};
  // Jitter spreads the retries of many calls that timed out together, so a recovering node sees
  // a ramp, not a spike.
  const double jitter = std::uniform_real_distribution<double>(0.5, 1.5)(rng);
  const auto delay = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
      retry_.base * (std::int64_t{1} << n) * jitter);
  {
    std::lock_guard lock(mu_);
    if (stopped_) return false;
    due_.emplace(std::chrono::steady_clock::now() + delay, std::move(retry));
  }
  wake_.notify_one();
  return true;
}

void ResilientBackend::RunTimer(std::stop_token stop) {
  using Clock = std::chrono::steady_clock;
  std::unique_lock lock(mu_);
  while (!stop.stop_requested()) {
    const auto now = Clock::now();
    std::vector<std::function<void(bool)>> ready;
    while (!due_.empty() && due_.begin()->first <= now) {
      ready.push_back(std::move(due_.begin()->second));
      due_.erase(due_.begin());
    }
    std::vector<std::shared_ptr<Pending>> expired;
    while (!deadlines_.empty() && deadlines_.front().first <= now) {
      expired.push_back(std::move(deadlines_.front().second));
      deadlines_.pop_front();
    }
    if (!ready.empty() || !expired.empty()) {
      lock.unlock();  // both call back into the caller or the inner backend, which may answer inline
      for (auto& r : ready) r(true);
      for (auto& p : expired) {
        if (First(p)) {
          deadlines_missed_.fetch_add(1, std::memory_order_relaxed);
          p->expire();
        }
      }
      expired.clear();  // drops what the attempts held, outside the lock
      lock.lock();
      continue;
    }
    // Sleep until the next retry, or the next deadline but at least kSweep from now: a busy backend
    // checks its deadlines in batches instead of waking this thread for every attempt.
    auto next = Clock::time_point::max();
    if (!due_.empty()) next = due_.begin()->first;
    if (!deadlines_.empty()) next = std::min(next, std::max(deadlines_.front().first, now + kSweep));
    // Wake early for a retry due sooner, or for the first deadline after none: a deadline added
    // behind others is never due before the front.
    const bool had_deadlines = !deadlines_.empty();
    const auto sooner = [this, next, had_deadlines] {
      return (!due_.empty() && due_.begin()->first < next) || (!had_deadlines && !deadlines_.empty());
    };
    if (next == Clock::time_point::max()) {
      wake_.wait(lock, stop, sooner);
    } else {
      wake_.wait_until(lock, stop, next, sooner);
    }
  }
}

}  // namespace ratelimiter
