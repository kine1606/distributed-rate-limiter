#include "redis/batcher.h"

#include <utility>

namespace ratelimiter {

Batcher::Batcher(RedisBackend& inner, Mode mode, std::chrono::microseconds max_delay, std::size_t max_batch)
    : inner_(inner), mode_(mode), max_delay_(max_delay), max_batch_(max_batch) {
  timer_ = std::thread([this] { RunTimer(); });
}

Batcher::~Batcher() {
  std::vector<Call> rest;
  {
    std::lock_guard lock(mu_);
    stop_ = true;
    rest = TakeQueueLocked();
  }
  timer_cv_.notify_one();
  timer_.join();
  Send(rest);
  std::unique_lock lock(mu_);
  idle_cv_.wait(lock, [this] { return in_flight_ == 0; });  // replies (or errors) always come back
}

std::vector<Batcher::Call> Batcher::TakeQueueLocked() {
  std::vector<Call> batch;
  batch.swap(queue_);
  in_flight_ += batch.size();
  return batch;
}

void Batcher::Eval(const Rule& rule, std::string_view key, std::uint32_t requested, ScriptCallback done) {
  std::vector<Call> batch;
  {
    std::lock_guard lock(mu_);
    queue_.push_back({rule, std::string(key), requested, std::move(done)});
    const bool idle = mode_ == Mode::kAdaptive && in_flight_ == 0;
    if (idle || queue_.size() >= max_batch_ || stop_) batch = TakeQueueLocked();
  }
  Send(batch);
}

void Batcher::Send(std::vector<Call>& batch) {
  // Issued back to back, so the inner client writes them in one go: the batch is one round trip.
  for (Call& c : batch) {
    inner_.Eval(c.rule, c.key, c.requested,
                [this, done = std::move(c.done)](const ScriptResult& r) mutable { OnReply(done, r); });
  }
}

void Batcher::OnReply(ScriptCallback& done, const ScriptResult& r) {
  std::vector<Call> next;
  bool idle = false;
  {
    std::lock_guard lock(mu_);
    --in_flight_;
    // Adaptive: the last outstanding reply is back, so whatever queued behind it goes now.
    if (mode_ == Mode::kAdaptive && in_flight_ == 0 && !queue_.empty()) next = TakeQueueLocked();
    idle = in_flight_ == 0;
  }
  if (idle) idle_cv_.notify_all();
  Send(next);
  done(r);
}

void Batcher::RunTimer() {
  std::unique_lock lock(mu_);
  while (!stop_) {
    timer_cv_.wait_for(lock, max_delay_);
    if (stop_ || queue_.empty()) continue;
    std::vector<Call> batch = TakeQueueLocked();
    lock.unlock();
    Send(batch);
    lock.lock();
  }
}

}  // namespace ratelimiter
