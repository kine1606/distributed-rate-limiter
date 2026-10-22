// Week 5 reliability: the circuit breaker on a fake clock, and ResilientBackend's breaker gating and
// retry rules on a fake Redis whose answers the test scripts.
#include "reliability/circuit_breaker.h"
#include "reliability/resilient_backend.h"
#include "reliability/slot_breakers.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ratelimiter {
namespace {

using std::chrono::milliseconds;
using State = CircuitBreaker::State;

std::int64_t g_now = 0;
std::int64_t FakeNow() { return g_now; }
constexpr std::int64_t kMs = 1'000'000;

class CircuitBreakerTest : public ::testing::Test {
 protected:
  void SetUp() override { g_now = 0; }
  void Fail(int n) {
    for (int i = 0; i < n; ++i) breaker.OnFailure();
  }
  CircuitBreaker breaker{{.failure_threshold = 20, .open_for = milliseconds(500)}, &FakeNow};
};

TEST_F(CircuitBreakerTest, OpensAfterTwentyFailuresInARow) {
  Fail(19);
  EXPECT_EQ(breaker.state(), State::kClosed);
  EXPECT_TRUE(breaker.Allow());
  EXPECT_TRUE(breaker.OnFailure());  // the 20th reports the change
  EXPECT_EQ(breaker.state(), State::kOpen);
  EXPECT_FALSE(breaker.Allow());
  EXPECT_FALSE(breaker.OnFailure());  // already open: no second report
}

TEST_F(CircuitBreakerTest, ASuccessStartsTheCountAgain) {
  Fail(19);
  EXPECT_FALSE(breaker.OnSuccess());  // was closed: no change to report
  Fail(19);
  EXPECT_EQ(breaker.state(), State::kClosed);
}

TEST_F(CircuitBreakerTest, OneProbeAfterTheOpenTimeThenItsAnswerDecides) {
  Fail(20);
  g_now = 499 * kMs;
  EXPECT_FALSE(breaker.Allow());
  g_now = 500 * kMs;
  EXPECT_TRUE(breaker.Allow());  // the probe
  EXPECT_EQ(breaker.state(), State::kHalfOpen);
  EXPECT_FALSE(breaker.Allow());  // only one
  EXPECT_TRUE(breaker.OnSuccess());
  EXPECT_EQ(breaker.state(), State::kClosed);
  EXPECT_TRUE(breaker.Allow());
}

// A success while open answers a call sent before it opened: stale, so only the probe decides.
TEST_F(CircuitBreakerTest, ALateSuccessDoesNotCloseAnOpenBreaker) {
  Fail(20);
  EXPECT_FALSE(breaker.OnSuccess());
  EXPECT_EQ(breaker.state(), State::kOpen);
  EXPECT_FALSE(breaker.Allow());
}

TEST_F(CircuitBreakerTest, AFailedProbeOpensItForAnotherPeriod) {
  Fail(20);
  g_now = 600 * kMs;
  ASSERT_TRUE(breaker.Allow());
  EXPECT_FALSE(breaker.OnFailure());  // re-opening is not news: no log line every probe
  EXPECT_EQ(breaker.state(), State::kOpen);
  g_now = 1099 * kMs;
  EXPECT_FALSE(breaker.Allow());  // 500 ms from the failed probe, not from the first opening
  g_now = 1100 * kMs;
  EXPECT_TRUE(breaker.Allow());
}

TEST(CircuitBreaker, ConcurrentUseLetsOneProbeThrough) {  // for TSan as well
  CircuitBreaker b({.failure_threshold = 1, .open_for = milliseconds(0)});
  b.OnFailure();
  std::atomic<int> probes{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 10'000; ++i) probes += b.Allow();
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(probes.load(), 1);  // nobody answered the probe: half-open lets nothing else through
}

// ---- ResilientBackend ----

// Answers each call with the next scripted result (the last one repeats), from the calling thread.
class ScriptedRedis final : public RedisBackend {
 public:
  using RedisBackend::Eval;
  void Eval(const Rule& rule, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    ScriptResult r;
    {
      std::lock_guard lock(mu);
      limits.push_back(rule.limit);
      r = Next(script);
    }
    done(r);
  }
  void Return(const Rule&, std::string_view, std::uint32_t, ScriptCallback done) override {
    ScriptResult r;
    {
      std::lock_guard lock(mu);
      ++returns;
      r = Next(script);
    }
    done(r);
  }
  void HGetAll(std::string, HashCallback done) override {
    HashResult h;
    {
      std::lock_guard lock(mu);
      ++reads;
      h = Next(hashes);
    }
    done(h);
  }
  int Calls() {
    std::lock_guard lock(mu);
    return static_cast<int>(limits.size());
  }
  template <typename T>
  static T Next(std::deque<T>& q) {
    T r = q.front();
    if (q.size() > 1) q.pop_front();
    return r;
  }

  std::mutex mu;
  std::deque<ScriptResult> script{ScriptResult{.allowed = true}};
  std::deque<HashResult> hashes{HashResult{}};
  std::vector<std::uint32_t> limits;
  int returns = 0, reads = 0;
};

const ScriptResult kTimeout{.redis_error = true, .timed_out = true};

class ResilientBackendTest : public ::testing::Test {
 protected:
  // Waits for the answer; retries come back on the backend's timer thread.
  ScriptResult EvalAndWait(const Rule& rule, std::string_view id = {}) {
    std::promise<ScriptResult> p;
    backend.Eval(rule, "k", 1, id, [&p](const ScriptResult& r) { p.set_value(r); });
    auto f = p.get_future();
    EXPECT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    return f.get();
  }

  ScriptedRedis redis;
  SlotBreakers breakers{{.failure_threshold = 6, .open_for = milliseconds(60'000)}};
  CircuitBreaker& breaker = breakers.ForEntity("k");  // every test call uses key "k"
  ResilientBackend backend{redis, breakers, {.max_retries = 2, .base = std::chrono::microseconds(100)}};
  const Rule bucket{.name = "b", .limit = 10, .refill_per_sec = 1};
  const Rule quota{.name = "q", .algorithm = Algorithm::kQuota, .limit = 10};
};

TEST_F(ResilientBackendTest, ATimedOutCallIsSentAgainAndCanSucceed) {
  redis.script = {kTimeout, {.allowed = true, .remaining = 4}};
  const auto r = EvalAndWait(bucket);
  EXPECT_FALSE(r.redis_error);
  EXPECT_EQ(r.remaining, 4u);
  EXPECT_EQ(redis.Calls(), 2);
}

TEST_F(ResilientBackendTest, AtMostTwoRetriesThenTheTimeoutIsTheAnswer) {
  redis.script = {kTimeout};
  const auto r = EvalAndWait(bucket);
  EXPECT_TRUE(r.redis_error);
  EXPECT_TRUE(r.timed_out);
  EXPECT_EQ(redis.Calls(), 3);
}

TEST_F(ResilientBackendTest, OtherErrorsAreNotRetried) {  // a script error would fail again
  redis.script = {{.redis_error = true}};
  EXPECT_TRUE(EvalAndWait(bucket).redis_error);
  EXPECT_EQ(redis.Calls(), 1);
}

// A timed-out quota script may have counted the call; sending it again would count it twice,
// unless a request id lets quota.lua recognise the repeat.
TEST_F(ResilientBackendTest, AnOpenBreakerAlsoStopsRetries) {
  SlotBreakers b({.failure_threshold = 2, .open_for = milliseconds(60'000)});
  ResilientBackend r(redis, b, {.max_retries = 2, .base = std::chrono::microseconds(100)});
  redis.script = {kTimeout};
  std::promise<ScriptResult> p;
  r.Eval(bucket, "k", 1, [&p](const ScriptResult& s) { p.set_value(s); });
  EXPECT_TRUE(p.get_future().get().redis_error);
  EXPECT_EQ(redis.Calls(), 2);  // the second timeout opened the breaker: no third attempt
}

// A timed-out quota script may have counted the call. Sent again it counts twice, or, with a request
// id, comes back replayed: then the admitted call is never recorded for billing (code review, Week 5).
TEST_F(ResilientBackendTest, QuotaScriptsAreNeverRetried) {
  redis.script = {kTimeout};
  EvalAndWait(quota);
  EvalAndWait(quota, "req-1");
  EXPECT_EQ(redis.Calls(), 2);
}

// MOVED or a script error means Redis answered: one bad rule must not open the breaker for everyone.
TEST_F(ResilientBackendTest, ErrorRepliesDoNotFeedTheBreaker) {
  redis.script = {{.redis_error = true, .reply_error = true}};
  for (int i = 0; i < 10; ++i) EXPECT_TRUE(EvalAndWait(bucket).redis_error);
  EXPECT_EQ(breaker.state(), State::kClosed);
  EXPECT_EQ(redis.Calls(), 10);  // and not retried: they would fail again
}

TEST_F(ResilientBackendTest, TokenReturnsAreNeverRetried) {  // a repeat could give tokens back twice
  redis.script = {kTimeout};
  std::promise<ScriptResult> p;
  backend.Return(bucket, "k", 2, [&p](const ScriptResult& r) { p.set_value(r); });
  EXPECT_TRUE(p.get_future().get().redis_error);
  EXPECT_EQ(redis.returns, 1);
}

TEST_F(ResilientBackendTest, PlanReadsAreRetried) {
  redis.hashes = {{.redis_error = true, .timed_out = true}, {.fields = {{"daily", "2000"}}}};
  std::promise<HashResult> p;
  backend.HGetAll("{c}:plan", [&p](const HashResult& h) { p.set_value(h); });
  auto f = p.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_EQ(f.get().fields.at("daily"), "2000");
  EXPECT_EQ(redis.reads, 2);
}

TEST_F(ResilientBackendTest, ARetryKeepsTheCallersRuleNumbers) {  // the caller's rule may be gone
  redis.script = {kTimeout, {.allowed = true}};
  {
    Rule from_plan = bucket;
    from_plan.limit = 77;
    EvalAndWait(from_plan);
  }
  EXPECT_EQ(redis.limits, (std::vector<std::uint32_t>{77, 77}));
}

TEST_F(ResilientBackendTest, AnOpenBreakerAnswersAtOnceWithoutCallingRedis) {
  redis.script = {{.redis_error = true}};
  for (int i = 0; i < 6; ++i) EvalAndWait(bucket);
  EXPECT_EQ(breaker.state(), State::kOpen);
  EXPECT_TRUE(EvalAndWait(bucket).redis_error);
  bool read_failed = false;
  backend.HGetAll("{k}:plan", [&](const HashResult& h) { read_failed = h.redis_error; });  // same slot as "k"
  EXPECT_TRUE(read_failed);
  EXPECT_EQ(redis.Calls(), 6);
  EXPECT_EQ(redis.reads, 0);
}

TEST_F(ResilientBackendTest, StoppingAnswersWaitingRetriesWithoutSendingThem) {
  ScriptedRedis slow;
  slow.script = {kTimeout};
  SlotBreakers b;
  ResilientBackend r(slow, b, {.max_retries = 2, .base = std::chrono::microseconds(10'000'000)});  // 10 s
  ScriptResult got;
  bool answered = false;
  r.Eval(bucket, "k", 1, [&](const ScriptResult& s) {
    got = s;
    answered = true;
  });
  EXPECT_FALSE(answered);  // waiting for its retry
  r.StopRetries();
  EXPECT_TRUE(answered);
  EXPECT_TRUE(got.redis_error);
  EXPECT_EQ(slow.Calls(), 1);
}

TEST_F(ResilientBackendTest, BreakerChangesAreReportedWithTheOpenGroupCount) {
  std::vector<std::size_t> seen;
  SlotBreakers b({.failure_threshold = 1, .open_for = milliseconds(0)});
  ResilientBackend r(redis, b, {.max_retries = 0}, [&](std::size_t open) { seen.push_back(open); });
  redis.script = {{.redis_error = true}, {.allowed = true}};
  r.Eval(bucket, "k", 1, [](const ScriptResult&) {});  // fails: its group opens
  r.Eval(bucket, "k", 1, [](const ScriptResult&) {});  // the probe succeeds: closes
  EXPECT_EQ(seen, (std::vector<std::size_t>{1, 0}));
}

// The Week 5 gate found it: with one breaker for the cluster, a dead master's failures opened it for
// every customer. A key in another slot group must still reach Redis.
TEST_F(ResilientBackendTest, ADeadSlotGroupDoesNotStopOtherGroups) {
  std::string other = "k0";
  for (int i = 1; &breakers.ForEntity(other) == &breaker; ++i) other = "k" + std::to_string(i);
  redis.script = {{.redis_error = true}};
  for (int i = 0; i < 6; ++i) EvalAndWait(bucket);
  EXPECT_EQ(breaker.state(), State::kOpen);
  EXPECT_EQ(breakers.Open(), 1u);
  redis.script = {{.allowed = true}};
  std::promise<ScriptResult> p;
  backend.Eval(bucket, other, 1, [&p](const ScriptResult& r) { p.set_value(r); });
  EXPECT_FALSE(p.get_future().get().redis_error);
  EXPECT_EQ(redis.Calls(), 7);  // it went out
}

TEST_F(ResilientBackendTest, AnOpenBreakerAnswersTokenReturnsAtOnce) {
  redis.script = {{.redis_error = true}};
  for (int i = 0; i < 6; ++i) EvalAndWait(bucket);
  ASSERT_EQ(breaker.state(), State::kOpen);
  std::promise<ScriptResult> p;
  backend.Return(bucket, "k", 3, [&p](const ScriptResult& r) { p.set_value(r); });
  EXPECT_TRUE(p.get_future().get().redis_error);
  std::lock_guard lock(redis.mu);
  EXPECT_EQ(redis.returns, 0);  // Redis was not asked
}

// The timeout itself opened the breaker: the scheduled retry finds it open and is not sent.
TEST(ResilientRetry, ATimedOutPlanReadIsNotRetriedOnceItsBreakerOpened) {
  ScriptedRedis redis;
  redis.hashes = {HashResult{.redis_error = true, .timed_out = true}};
  SlotBreakers breakers({.failure_threshold = 1, .open_for = milliseconds(60'000)});
  ResilientBackend backend(redis, breakers, {.max_retries = 2, .base = std::chrono::microseconds(100)});
  std::promise<HashResult> p;
  backend.HGetAll("{cust:1}:plan", [&p](const HashResult& h) { p.set_value(h); });
  EXPECT_TRUE(p.get_future().get().redis_error);
  std::lock_guard lock(redis.mu);
  EXPECT_EQ(redis.reads, 1);
}

// ---- Per-attempt deadline (Week 6) ----

// Holds the callbacks of the first `hold` calls, as a slow or silent Redis would; later calls are
// answered at once with `reply`. The test answers held calls itself, late.
class HoldingRedis final : public RedisBackend {
 public:
  explicit HoldingRedis(int hold) : hold_(hold) {}
  using RedisBackend::Eval;
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override { Take(std::move(done)); }
  void Return(const Rule&, std::string_view, std::uint32_t, ScriptCallback done) override { Take(std::move(done)); }
  void HGetAll(std::string, HashCallback done) override {
    std::unique_lock lock(mu);
    if (calls++ < hold_) {
      held_hashes.push_back(std::move(done));
      return;
    }
    lock.unlock();
    done({});
  }
  void AnswerHeld(const ScriptResult& r) {  // the late replies
    std::vector<ScriptCallback> held;
    {
      std::lock_guard lock(mu);
      held.swap(held_scripts);
    }
    for (auto& cb : held) cb(r);
  }

  std::mutex mu;
  int calls = 0;
  ScriptResult reply{.allowed = true, .remaining = 7};
  std::vector<ScriptCallback> held_scripts;
  std::vector<HashCallback> held_hashes;

 private:
  void Take(ScriptCallback done) {
    std::unique_lock lock(mu);
    if (calls++ < hold_) {
      held_scripts.push_back(std::move(done));
      return;
    }
    lock.unlock();
    done(reply);
  }
  const int hold_;
};

// Counts the answers one call gets and keeps the first.
struct Answers {
  std::mutex mu;
  int count = 0;
  std::promise<ScriptResult> first;
  ScriptCallback Callback() {
    return [this](const ScriptResult& r) {
      std::lock_guard lock(mu);
      if (count++ == 0) first.set_value(r);
    };
  }
  int Count() {
    std::lock_guard lock(mu);
    return count;
  }
};

const Rule kBucket{.name = "b", .limit = 10, .refill_per_sec = 1};
const Rule kQuota{.name = "q", .algorithm = Algorithm::kQuota, .limit = 10};

// The Week 6 fault test found it: hiredis's timeout is an idle timeout per connection, so a master
// answering every call 80 ms late under load never tripped it. The deadline bounds each attempt.
TEST(ResilientDeadline, AnAttemptRedisDoesNotAnswerIsTimedOutAtItsDeadlineAndItsLateReplyDropped) {
  HoldingRedis redis(1);
  SlotBreakers breakers{{.failure_threshold = 1, .open_for = milliseconds(60'000)}};
  ResilientBackend backend(redis, breakers, {.max_retries = 0, .deadline = milliseconds(20)});
  Answers a;
  const auto start = std::chrono::steady_clock::now();
  backend.Eval(kQuota, "k", 1, a.Callback());
  auto f = a.first.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const ScriptResult r = f.get();
  EXPECT_GE(std::chrono::steady_clock::now() - start, milliseconds(20));
  EXPECT_TRUE(r.redis_error);
  EXPECT_TRUE(r.timed_out);
  EXPECT_EQ(breakers.ForEntity("k").state(), State::kOpen);  // counted as a failure
  EXPECT_EQ(backend.DeadlinesMissed(), 1u);
  redis.AnswerHeld({.allowed = true});  // the reply arrives after all
  EXPECT_EQ(a.Count(), 1);              // dropped: the caller had its answer
  EXPECT_EQ(breakers.ForEntity("k").state(), State::kOpen);  // and it does not count as a success
}

TEST(ResilientDeadline, AReplyBeforeTheDeadlineWinsAndTheDeadlineThenDoesNothing) {
  HoldingRedis redis(0);
  SlotBreakers breakers;
  ResilientBackend backend(redis, breakers, {.max_retries = 0, .deadline = milliseconds(20)});
  Answers a;
  backend.Eval(kQuota, "k", 1, a.Callback());
  EXPECT_EQ(a.first.get_future().get().remaining, 7u);
  std::this_thread::sleep_for(milliseconds(60));  // well past the deadline
  EXPECT_EQ(a.Count(), 1);
  EXPECT_EQ(backend.DeadlinesMissed(), 0u);
}

TEST(ResilientDeadline, AnAttemptThatMissesItsDeadlineIsRetriedWhereSafe) {
  HoldingRedis redis(1);  // the first attempt hangs, the retry is answered
  SlotBreakers breakers;
  ResilientBackend backend(redis, breakers, {.max_retries = 2, .base = std::chrono::microseconds(100),
                                             .deadline = milliseconds(20)});
  Answers a;
  backend.Eval(kBucket, "k", 1, a.Callback());
  const ScriptResult r = a.first.get_future().get();
  EXPECT_FALSE(r.redis_error);
  EXPECT_EQ(r.remaining, 7u);
  EXPECT_EQ(redis.calls, 2);
  EXPECT_EQ(backend.DeadlinesMissed(), 1u);
  redis.AnswerHeld({.allowed = true, .remaining = 1});  // the first attempt's late reply
  EXPECT_EQ(a.Count(), 1);
}

TEST(ResilientDeadline, APlanReadRedisDoesNotAnswerTimesOut) {
  HoldingRedis redis(1);
  SlotBreakers breakers;
  ResilientBackend backend(redis, breakers, {.max_retries = 0, .deadline = milliseconds(20)});
  std::promise<HashResult> p;
  backend.HGetAll("{cust:1}:plan", [&p](const HashResult& h) { p.set_value(h); });
  auto f = p.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  const HashResult h = f.get();
  EXPECT_TRUE(h.redis_error);
  EXPECT_TRUE(h.timed_out);
}

TEST(ResilientDeadline, StoppingAnswersAttemptsStillWaitingForTheirDeadline) {  // at shutdown nobody waits
  HoldingRedis redis(1);
  SlotBreakers breakers;
  ResilientBackend backend(redis, breakers, {.max_retries = 0, .deadline = std::chrono::seconds(30)});
  Answers a;
  backend.Eval(kQuota, "k", 1, a.Callback());
  backend.StopRetries();
  auto f = a.first.get_future();
  ASSERT_EQ(f.wait_for(std::chrono::seconds(2)), std::future_status::ready);
  EXPECT_TRUE(f.get().timed_out);
  redis.AnswerHeld({.allowed = true});
  EXPECT_EQ(a.Count(), 1);
}

// The deadline queue is lock-free (many threads add, the timer thread takes out). Eight threads send
// attempts that Redis answers at once, answers late from another thread, or never answers: each must
// be answered exactly once, by its reply or by its deadline. Run under TSan too.
TEST(ResilientDeadline, ManyThreadsEveryAttemptAnsweredExactlyOnce) {
  class MixedRedis final : public RedisBackend {
   public:
    using RedisBackend::Eval;
    void Eval(const Rule&, std::string_view, std::uint32_t requested, std::string_view, ScriptCallback done) override {
      if (requested % 3 == 0) {
        done({.allowed = true});  // at once
      } else if (requested % 3 == 1) {
        std::lock_guard lock(mu);
        late.push_back(std::move(done));  // later, from another thread
      }  // else never: only the deadline answers
    }
    void HGetAll(std::string, HashCallback done) override { done({}); }
    std::mutex mu;
    std::vector<ScriptCallback> late;
  } redis;
  SlotBreakers breakers{{.failure_threshold = 1'000'000, .open_for = milliseconds(1)}};
  ResilientBackend backend(redis, breakers, {.max_retries = 0, .deadline = milliseconds(5)});
  constexpr int kThreads = 8, kPerThread = 3000;
  std::vector<std::atomic<int>> answers(kThreads * kPerThread);
  std::atomic<bool> sending{true};
  std::thread replier([&] {  // answers the held calls while the senders run, and once more after
    for (bool last = false; !last;) {
      last = !sending.load();
      std::vector<ScriptCallback> batch;
      {
        std::lock_guard lock(redis.mu);
        batch.swap(redis.late);
      }
      for (auto& d : batch) d({.allowed = true});
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });
  std::vector<std::thread> senders;
  for (int t = 0; t < kThreads; ++t) {
    senders.emplace_back([&, t] {
      for (int i = 0; i < kPerThread; ++i) {
        const int id = t * kPerThread + i;
        backend.Eval(kQuota, "k" + std::to_string(id % 97), static_cast<std::uint32_t>(id),
                     [&answers, id](const ScriptResult&) { answers[static_cast<std::size_t>(id)].fetch_add(1); });
      }
    });
  }
  for (auto& s : senders) s.join();
  std::this_thread::sleep_for(milliseconds(50));  // past every deadline
  sending = false;
  replier.join();
  int never = 0, twice = 0;
  for (auto& a : answers) {
    never += a.load() == 0;
    twice += a.load() > 1;
  }
  EXPECT_EQ(never, 0);
  EXPECT_EQ(twice, 0);
  EXPECT_GT(backend.DeadlinesMissed(), 0u);
}

TEST(ResilientDeadline, WithoutADeadlineOnlyTheReplyAnswers) {
  HoldingRedis redis(1);
  SlotBreakers breakers;
  ResilientBackend backend(redis, breakers, {.max_retries = 0});
  Answers a;
  backend.Eval(kQuota, "k", 1, a.Callback());
  std::this_thread::sleep_for(milliseconds(30));
  EXPECT_EQ(a.Count(), 0);
  redis.AnswerHeld({.allowed = true});
  EXPECT_EQ(a.Count(), 1);
}

// Slots as Redis computes them (redis.io cluster specification and CLUSTER KEYSLOT).
TEST(SlotBreakers, HashSlotFollowsRedis) {
  EXPECT_EQ(HashSlot("123456789"), 12739);  // CRC16/XMODEM check value 0x31C3
  EXPECT_EQ(HashSlot("{user1000}.following"), HashSlot("{user1000}.followers"));
  EXPECT_EQ(HashSlot("{user1000}.following"), HashSlot("user1000"));
  EXPECT_EQ(HashSlot("foo{}{bar}"), Crc16Slot("foo{}{bar}"));  // an empty tag: the whole key
  EXPECT_EQ(HashSlot("foo{{bar}}zap"), Crc16Slot("{bar"));     // the tag ends at the first }
  SlotBreakers b;
  EXPECT_EQ(&b.ForEntity("cust:42"), &b.ForKey("{cust:42}:daily_quota:q"));  // keys.h builds this form
}

}  // namespace
}  // namespace ratelimiter
