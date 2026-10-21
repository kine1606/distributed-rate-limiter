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
  int retries = 0;
};

struct ResilientBackend::HashCall {
  std::string key;
  HashCallback done;
  int retries = 0;
};

ResilientBackend::ResilientBackend(RedisBackend& inner, CircuitBreaker& breaker, RetryOptions retry,
                                   StateObserver on_change)
    : inner_(inner), breaker_(breaker), retry_(retry), on_change_(std::move(on_change)) {
  timer_ = std::jthread([this](std::stop_token stop) { RunTimer(stop); });
}

ResilientBackend::~ResilientBackend() { StopRetries(); }

void ResilientBackend::StopRetries() {
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> left;
  {
    std::lock_guard lock(mu_);
    if (stopped_) return;
    stopped_ = true;
    left.swap(due_);
  }
  timer_.request_stop();
  if (timer_.joinable()) timer_.join();
  for (auto& [when, retry] : left) retry(false);
}

void ResilientBackend::Account(bool failed) {
  if ((failed ? breaker_.OnFailure() : breaker_.OnSuccess()) && on_change_) on_change_(breaker_.state());
}

void ResilientBackend::Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                            std::string_view request_id, ScriptCallback done) {
  if (!breaker_.Allow()) {
    done({.redis_error = true});
    return;
  }
  const bool retryable = rule.algorithm != Algorithm::kQuota || !request_id.empty();
  if (!retryable || retry_.max_retries == 0) {
    inner_.Eval(rule, key, requested, request_id, [this, done = std::move(done)](const ScriptResult& r) {
      Account(r.redis_error);
      done(r);
    });
    return;
  }
  Send(std::make_shared<ScriptCall>(
      ScriptCall{rule, std::string(key), requested, std::string(request_id), std::move(done)}));
}

void ResilientBackend::Send(const std::shared_ptr<ScriptCall>& c) {
  inner_.Eval(c->rule, c->key, c->requested, c->request_id, [this, c](const ScriptResult& r) {
    Account(r.redis_error);
    if (r.timed_out && c->retries < retry_.max_retries &&
        Schedule(c->retries++, [this, c](bool run) {
          if (run && breaker_.Allow()) {
            Send(c);
          } else {
            c->done({.redis_error = true});
          }
        })) {
      return;
    }
    c->done(r);
  });
}

void ResilientBackend::Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) {
  if (!breaker_.Allow()) {
    done({.redis_error = true});
    return;
  }
  inner_.Return(rule, key, tokens, [this, done = std::move(done)](const ScriptResult& r) {
    Account(r.redis_error);
    done(r);
  });
}

void ResilientBackend::HGetAll(std::string key, HashCallback done) {
  if (!breaker_.Allow()) {
    done({.redis_error = true});
    return;
  }
  Send(std::make_shared<HashCall>(HashCall{std::move(key), std::move(done)}));
}

void ResilientBackend::Send(const std::shared_ptr<HashCall>& c) {
  inner_.HGetAll(c->key, [this, c](const HashResult& h) {
    Account(h.redis_error);
    if (h.timed_out && c->retries < retry_.max_retries &&
        Schedule(c->retries++, [this, c](bool run) {
          if (run && breaker_.Allow()) {
            Send(c);
          } else {
            c->done({.redis_error = true});
          }
        })) {
      return;
    }
    c->done(h);
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
  std::unique_lock lock(mu_);
  while (!stop.stop_requested()) {
    if (due_.empty()) {
      wake_.wait(lock, stop, [this] { return !due_.empty(); });
      continue;
    }
    const auto next = due_.begin()->first;
    if (std::chrono::steady_clock::now() < next) {
      wake_.wait_until(lock, stop, next, [this, next] { return !due_.empty() && due_.begin()->first < next; });
      continue;
    }
    std::vector<std::function<void(bool)>> ready;
    while (!due_.empty() && due_.begin()->first <= std::chrono::steady_clock::now()) {
      ready.push_back(std::move(due_.begin()->second));
      due_.erase(due_.begin());
    }
    lock.unlock();  // a retry calls the inner backend, which may answer inline
    for (auto& r : ready) r(true);
    lock.lock();
  }
}

}  // namespace ratelimiter
