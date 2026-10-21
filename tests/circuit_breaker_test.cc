// Week 5 reliability: the circuit breaker on a fake clock, and ResilientBackend's breaker gating and
// retry rules on a fake Redis whose answers the test scripts.
#include "reliability/circuit_breaker.h"
#include "reliability/resilient_backend.h"

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

TEST_F(CircuitBreakerTest, AFailedProbeOpensItForAnotherPeriod) {
  Fail(20);
  g_now = 600 * kMs;
  ASSERT_TRUE(breaker.Allow());
  EXPECT_TRUE(breaker.OnFailure());
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
  CircuitBreaker breaker{{.failure_threshold = 6, .open_for = milliseconds(60'000)}};
  ResilientBackend backend{redis, breaker, {.max_retries = 2, .base = std::chrono::microseconds(100)}};
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
  CircuitBreaker b({.failure_threshold = 2, .open_for = milliseconds(60'000)});
  ResilientBackend r(redis, b, {.max_retries = 2, .base = std::chrono::microseconds(100)});
  redis.script = {kTimeout};
  std::promise<ScriptResult> p;
  r.Eval(bucket, "k", 1, [&p](const ScriptResult& s) { p.set_value(s); });
  EXPECT_TRUE(p.get_future().get().redis_error);
  EXPECT_EQ(redis.Calls(), 2);  // the second timeout opened the breaker: no third attempt
}

TEST_F(ResilientBackendTest, QuotaIsRetriedOnlyWithARequestId) {
  redis.script = {kTimeout};
  EvalAndWait(quota);
  EXPECT_EQ(redis.Calls(), 1);
  EvalAndWait(quota, "req-1");
  EXPECT_EQ(redis.Calls(), 1 + 3);
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
  backend.HGetAll("{c}:plan", [&](const HashResult& h) { read_failed = h.redis_error; });
  EXPECT_TRUE(read_failed);
  EXPECT_EQ(redis.Calls(), 6);
  EXPECT_EQ(redis.reads, 0);
}

TEST_F(ResilientBackendTest, StoppingAnswersWaitingRetriesWithoutSendingThem) {
  ScriptedRedis slow;
  slow.script = {kTimeout};
  CircuitBreaker b;
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

TEST_F(ResilientBackendTest, BreakerChangesAreReported) {
  std::vector<State> seen;
  CircuitBreaker b({.failure_threshold = 1, .open_for = milliseconds(0)});
  ResilientBackend r(redis, b, {.max_retries = 0}, [&](State s) { seen.push_back(s); });
  redis.script = {{.redis_error = true}, {.allowed = true}};
  r.Eval(bucket, "k", 1, [](const ScriptResult&) {});  // fails: opens
  r.Eval(bucket, "k", 1, [](const ScriptResult&) {});  // the probe succeeds: closes
  EXPECT_EQ(seen, (std::vector<State>{State::kOpen, State::kClosed}));
}

}  // namespace
}  // namespace ratelimiter
