#include "reliability/resilient_backend.h"

#include <algorithm>
#include <deque>
#include <iterator>
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

struct ResilientBackend::HashCall {
  std::string key;
  HashCallback done;
  CircuitBreaker* breaker;
  int retries = 0;
};

struct ResilientBackend::Pending {
  virtual ~Pending() = default;
  virtual void Expire() {}  // answers the attempt as timed out; the queue's placeholder does nothing
  bool Claim() { return !answered.exchange(true, std::memory_order_acq_rel); }  // false: answered already

  std::atomic<Pending*> next{nullptr};  // the queue's link
  std::chrono::steady_clock::time_point due{};
  std::atomic<bool> answered{false};
  std::shared_ptr<Pending> self;  // the queue's reference, until the timer thread takes it out
};

namespace {

template <typename Result>
Result TimedOut() {
  Result r;
  r.redis_error = true;
  r.timed_out = true;
  return r;
}

constexpr auto kSweep = std::chrono::milliseconds(5);  // how often deadlines are checked, at most

}  // namespace

// One allocation per attempt: the node holds the answer itself.
template <typename Result, typename Answer>
struct ResilientBackend::Attempt final : Pending {
  explicit Attempt(Answer a) : answer(std::move(a)) {}
  void Expire() override { answer(TimedOut<Result>()); }
  Answer answer;
};

ResilientBackend::ResilientBackend(RedisBackend& inner, SlotBreakers& breakers, RetryOptions retry,
                                   StateObserver on_change)
    : inner_(inner),
      breakers_(breakers),
      retry_(retry),
      on_change_(std::move(on_change)),
      stub_(std::make_unique<Pending>()),
      head_(stub_.get()),
      tail_(stub_.get()) {
  timer_ = std::jthread([this](std::stop_token stop) { RunTimer(stop); });
}

ResilientBackend::~ResilientBackend() {
  StopRetries();
  for (Pending* p; (p = Pop()) != nullptr;) p->self.reset();  // added after the stop: free them
}

void ResilientBackend::StopRetries() {
  std::multimap<std::chrono::steady_clock::time_point, std::function<void(bool)>> left;
  {
    std::lock_guard lock(mu_);
    if (stopped_.load(std::memory_order_relaxed)) return;
    stopped_.store(true, std::memory_order_relaxed);
    left.swap(due_);
  }
  timer_.request_stop();  // it answers the attempts it holds as it stops
  if (timer_.joinable()) timer_.join();
  for (auto& [when, retry] : left) retry(false);
  for (Pending* p; (p = Pop()) != nullptr;) {  // added while it stopped
    const std::shared_ptr<Pending> held = std::move(p->self);
    if (held->Claim()) held->Expire();
  }
}

// Vyukov's intrusive MPSC queue (1024cores.net, "Intrusive MPSC node-based queue").
void ResilientBackend::Push(Pending* p) {
  p->next.store(nullptr, std::memory_order_relaxed);
  Pending* prev = head_.exchange(p, std::memory_order_seq_cst);  // seq_cst: pairs with idle_ (Watch, RunTimer)
  prev->next.store(p, std::memory_order_release);
}

ResilientBackend::Pending* ResilientBackend::Pop() {
  Pending* tail = tail_;
  Pending* next = tail->next.load(std::memory_order_acquire);
  if (tail == stub_.get()) {
    if (next == nullptr) return nullptr;
    tail_ = tail = next;
    next = next->next.load(std::memory_order_acquire);
  }
  if (next != nullptr) {
    tail_ = next;
    return tail;
  }
  if (tail != head_.load(std::memory_order_acquire)) return nullptr;  // a push is between its two steps
  Push(stub_.get());
  next = tail->next.load(std::memory_order_acquire);
  if (next != nullptr) {
    tail_ = next;
    return tail;
  }
  return nullptr;
}

bool ResilientBackend::QueueEmpty() const {
  return tail_ == stub_.get() && stub_->next.load(std::memory_order_seq_cst) == nullptr &&
         head_.load(std::memory_order_seq_cst) == stub_.get();
}

template <typename Result, typename Answer, typename SendFn>
void ResilientBackend::Watch(Answer answer, SendFn send) {
  if (retry_.deadline.count() == 0 || stopped_.load(std::memory_order_relaxed)) {
    send([answer = std::move(answer)](const Result& r) { answer(r); });
    return;
  }
  auto a = std::make_shared<Attempt<Result, Answer>>(std::move(answer));
  a->due = std::chrono::steady_clock::now() + retry_.deadline;
  a->self = a;
  Push(a.get());
  if (idle_.load(std::memory_order_seq_cst)) {  // the timer sleeps with nothing to watch: wake it
    std::lock_guard lock(mu_);
    idle_.store(false, std::memory_order_seq_cst);
    wake_.notify_one();
  }
  send([a = std::move(a)](const Result& r) {
    if (a->Claim()) a->answer(r);  // else the deadline answered: this reply is late
  });
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
    Watch<ScriptResult>(
        [this, &breaker, done = std::move(done)](const ScriptResult& r) {
          Account(breaker, r);
          done(r);
        },
        [&](ScriptCallback reply) { inner_.Eval(rule, key, requested, request_id, std::move(reply)); });
    return;
  }
  Send(std::make_shared<ScriptCall>(
      ScriptCall{rule, std::string(key), requested, std::string(request_id), std::move(done), &breaker}));
}

void ResilientBackend::Send(const std::shared_ptr<ScriptCall>& c) {
  Watch<ScriptResult>(
      [this, c](const ScriptResult& r) {
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
      },
      [&](ScriptCallback reply) { inner_.Eval(c->rule, c->key, c->requested, c->request_id, std::move(reply)); });
}

void ResilientBackend::Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) {
  CircuitBreaker& breaker = breakers_.ForEntity(key);
  if (!breaker.Allow()) {
    done({.redis_error = true});
    return;
  }
  Watch<ScriptResult>(
      [this, &breaker, done = std::move(done)](const ScriptResult& r) {
        Account(breaker, r);
        done(r);
      },
      [&](ScriptCallback reply) { inner_.Return(rule, key, tokens, std::move(reply)); });
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
  Watch<HashResult>(
      [this, c](const HashResult& h) {
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
      },
      [&](HashCallback reply) { inner_.HGetAll(c->key, std::move(reply)); });
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
    if (stopped_.load(std::memory_order_relaxed)) return false;
    due_.emplace(std::chrono::steady_clock::now() + delay, std::move(retry));
  }
  wake_.notify_one();
  return true;
}

void ResilientBackend::RunTimer(std::stop_token stop) {
  using Clock = std::chrono::steady_clock;
  std::deque<std::shared_ptr<Pending>> waiting;  // taken out of the queue, in send order: due order
  const auto take = [&] {
    for (Pending* p; (p = Pop()) != nullptr;) waiting.push_back(std::move(p->self));
  };
  std::unique_lock lock(mu_);
  while (!stop.stop_requested()) {
    lock.unlock();  // answers call back into the caller or the inner backend, which may answer inline
    take();
    const auto now = Clock::now();
    const auto due = std::find_if(waiting.begin(), waiting.end(), [now](const auto& p) { return p->due > now; });
    std::vector<std::shared_ptr<Pending>> expired(std::make_move_iterator(waiting.begin()), std::make_move_iterator(due));
    waiting.erase(waiting.begin(), due);
    for (const auto& p : expired) {
      if (p->Claim()) {
        deadlines_missed_.fetch_add(1, std::memory_order_relaxed);
        p->Expire();
      }
    }
    expired.clear();  // the attempts the reply already answered just go
    lock.lock();
    std::vector<std::function<void(bool)>> ready;
    while (!due_.empty() && due_.begin()->first <= now) {
      ready.push_back(std::move(due_.begin()->second));
      due_.erase(due_.begin());
    }
    if (!ready.empty()) {
      lock.unlock();
      for (auto& r : ready) r(true);
      lock.lock();
      continue;
    }
    // Sleep until the next retry, or the next deadline but at least kSweep from now; attempts added
    // meanwhile are due later than those already waiting, so they need no wake-up.
    auto next = Clock::time_point::max();
    if (!due_.empty()) next = due_.begin()->first;
    if (!waiting.empty()) {
      next = std::min(next, std::max(waiting.front()->due, now + kSweep));
    } else if (!QueueEmpty()) {
      next = std::min(next, now + kSweep);  // an attempt is being added
    }
    if (next == Clock::time_point::max()) {  // nothing to watch: sleep until an attempt or retry comes
      idle_.store(true, std::memory_order_seq_cst);
      if (!QueueEmpty()) {  // added between the check above and `idle_`: it may not have woken us
        idle_.store(false, std::memory_order_seq_cst);
        continue;
      }
      wake_.wait(lock, stop, [this] { return !idle_.load(std::memory_order_seq_cst) || !due_.empty(); });
      idle_.store(false, std::memory_order_seq_cst);
    } else {
      wake_.wait_until(lock, stop, next, [this, next] { return !due_.empty() && due_.begin()->first < next; });
    }
  }
  lock.unlock();
  take();  // stopping: nobody may wait for a deadline any more
  for (const auto& p : waiting) {
    if (p->Claim()) p->Expire();
  }
}

}  // namespace ratelimiter
