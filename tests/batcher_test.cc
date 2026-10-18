#include "redis/batcher.h"

#include <gtest/gtest.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ratelimiter {
namespace {

using std::chrono::milliseconds;

// Holds every call until the test answers it, or answers at once when `inline_replies` is set.
class FakeInner final : public RedisBackend {
 public:
  void Eval(const Rule&, std::string_view, std::uint32_t requested, std::string_view, ScriptCallback done) override {
    if (inline_replies) {
      Answer(requested, done);
      return;
    }
    std::lock_guard lock(mu);
    pending.push_back({requested, std::move(done)});
  }
  void HGetAll(std::string, HashCallback done) override { done({}); }

  std::size_t Sent() {
    std::lock_guard lock(mu);
    return sent + pending.size();
  }
  void ReplyOldest() {
    std::pair<std::uint32_t, ScriptCallback> c;
    {
      std::lock_guard lock(mu);
      c = std::move(pending.front());
      pending.pop_front();
      ++sent;
    }
    Answer(c.first, c.second);
  }
  static void Answer(std::uint32_t requested, ScriptCallback& done) {
    ScriptResult r;
    r.allowed = true;
    r.remaining = requested;  // echo, so each caller can check it got its own reply
    done(r);
  }

  bool inline_replies = false;
  std::mutex mu;
  std::deque<std::pair<std::uint32_t, ScriptCallback>> pending;
  std::size_t sent = 0;
};

const Rule kRule{.name = "b", .limit = 100, .refill_per_sec = 10};

bool WaitFor(const std::function<bool()>& ok, milliseconds limit = milliseconds(2000)) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (!ok()) {
    if (std::chrono::steady_clock::now() > end) return false;
    std::this_thread::sleep_for(milliseconds(1));
  }
  return true;
}

TEST(Batcher, TimerModeHoldsCallsUntilTheBatchIsFullOrTheTimerFires) {
  FakeInner inner;
  Batcher b(inner, Batcher::Mode::kTimer, std::chrono::microseconds(50'000), 3);
  b.Eval(kRule, "k", 1, [](const ScriptResult&) {});
  b.Eval(kRule, "k", 2, [](const ScriptResult&) {});
  EXPECT_EQ(inner.Sent(), 0u);  // neither full nor timed out yet (50 ms)
  b.Eval(kRule, "k", 3, [](const ScriptResult&) {});
  EXPECT_EQ(inner.Sent(), 3u);  // full batch: sent at once
  b.Eval(kRule, "k", 4, [](const ScriptResult&) {});
  EXPECT_TRUE(WaitFor([&] { return inner.Sent() == 4u; }));  // the timer sends the straggler
  while (!inner.pending.empty()) inner.ReplyOldest();
}

TEST(Batcher, AdaptiveModeSendsALoneCallAtOnceAndQueuesOnlyBehindReplies) {
  FakeInner inner;
  Batcher b(inner, Batcher::Mode::kAdaptive, std::chrono::microseconds(1'000'000), 128);
  std::vector<std::uint32_t> got;
  const auto record = [&got](const ScriptResult& r) { got.push_back(r.remaining); };
  b.Eval(kRule, "k", 1, record);
  EXPECT_EQ(inner.Sent(), 1u);  // nothing in flight: no waiting for the timer
  b.Eval(kRule, "k", 2, record);
  b.Eval(kRule, "k", 3, record);
  EXPECT_EQ(inner.Sent(), 1u);  // queued behind the outstanding call
  inner.ReplyOldest();
  EXPECT_EQ(inner.Sent(), 3u);  // the last reply released the queue as one batch
  inner.ReplyOldest();
  inner.ReplyOldest();
  EXPECT_EQ(got, (std::vector<std::uint32_t>{1, 2, 3}));  // every caller got its own reply
}

TEST(Batcher, DestructorSendsTheQueueAndWaitsForEveryReply) {
  FakeInner inner;
  inner.inline_replies = true;
  int answered = 0;
  {
    Batcher b(inner, Batcher::Mode::kTimer, std::chrono::microseconds(10'000'000), 128);  // timer never fires
    b.Eval(kRule, "k", 1, [&](const ScriptResult&) { ++answered; });
    b.Eval(kRule, "k", 2, [&](const ScriptResult&) { ++answered; });
    EXPECT_EQ(answered, 0);
  }
  EXPECT_EQ(answered, 2);
}

TEST(Batcher, ConcurrentCallersAllGetAnAnswer) {
  FakeInner inner;
  inner.inline_replies = true;
  std::atomic<int> answered{0};
  {
    Batcher b(inner, Batcher::Mode::kAdaptive, std::chrono::microseconds(200), 16);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
      threads.emplace_back([&] {
        for (int i = 0; i < 500; ++i) b.Eval(kRule, "k", 1, [&](const ScriptResult&) { ++answered; });
      });
    }
    for (auto& t : threads) t.join();
  }
  EXPECT_EQ(answered.load(), 4000);
}

// StrictCore passes a temporary rule with the plan's numbers: the batch must hold its own copy, and
// the request id must survive the queue.
TEST(Batcher, QueuedCallsKeepTheirRuleNumbersAndRequestId) {
  struct Recorder final : RedisBackend {
    using RedisBackend::Eval;
    void Eval(const Rule& rule, std::string_view key, std::uint32_t, std::string_view id,
              ScriptCallback done) override {
      seen.push_back(rule.name + "/" + std::to_string(rule.limit) + "/" + std::string(key) + "/" + std::string(id));
      done({.allowed = true});
    }
    void Return(const Rule& rule, std::string_view, std::uint32_t tokens, ScriptCallback done) override {
      seen.push_back("return/" + rule.name + "/" + std::to_string(tokens));
      done({.allowed = true});
    }
    void HGetAll(std::string key, HashCallback done) override {
      seen.push_back("hgetall/" + key);
      done({});
    }
    std::vector<std::string> seen;
  } inner;
  {
    Batcher b(inner, Batcher::Mode::kTimer, std::chrono::microseconds(10'000'000), 128);  // sent at destruction
    {
      Rule plan_numbers = kRule;
      plan_numbers.limit = 77;
      std::string key = "cust:42";
      b.Eval(plan_numbers, key, 1, "id-1", [](const ScriptResult&) {});
    }  // the caller's rule and key are gone before the batch goes out
    b.Return(kRule, "cust:42", 2, [](const ScriptResult&) {});    // not batched: straight through
    b.HGetAll("{cust:42}:plan", [](const HashResult&) {});         // neither
    EXPECT_EQ(inner.seen, (std::vector<std::string>{"return/b/2", "hgetall/{cust:42}:plan"}));
  }
  EXPECT_EQ(inner.seen.back(), "b/77/cust:42/id-1");
}

}  // namespace
}  // namespace ratelimiter
