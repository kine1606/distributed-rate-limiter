#include <gtest/gtest.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/leased_core.h"

namespace ratelimiter {
namespace {

// Holds every Eval until the test answers it, so a test decides when "Redis" replies.
class ManualBackend final : public RedisBackend {
 public:
  struct Call {
    std::string rule;
    std::string key;
    std::uint32_t requested;
    ScriptCallback done;
  };

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, ScriptCallback done) override {
    std::lock_guard lock(mu_);
    pending_.push_back({rule.name, std::string(key), requested, std::move(done)});
    ++total_;
  }

  // Answers the oldest pending call; returns what it had asked for.
  std::uint32_t Reply(ScriptResult r) {
    Call c;
    {
      std::lock_guard lock(mu_);
      c = std::move(pending_.front());
      pending_.pop_front();
    }
    c.done(r);
    return c.requested;
  }

  std::uint32_t PendingRequested() {
    std::lock_guard lock(mu_);
    return pending_.empty() ? 0 : pending_.front().requested;
  }
  std::size_t Pending() {
    std::lock_guard lock(mu_);
    return pending_.size();
  }
  int Total() {
    std::lock_guard lock(mu_);
    return total_;
  }

 private:
  std::mutex mu_;
  std::deque<Call> pending_;
  int total_ = 0;
};

ScriptResult Granted() { return {.allowed = true}; }
ScriptResult Refused(std::uint32_t remaining, std::uint32_t retry_after_ms) {
  return {.allowed = false, .remaining = remaining, .retry_after_ms = retry_after_ms};
}
ScriptResult Down() { return {.redis_error = true}; }

RuleTable Rules() {
  // limit 100 -> chunk 10, early refill below 3 tokens
  Rule burst{.name = "burst", .limit = 100, .refill_per_sec = 10, .failure_policy = FailurePolicy::kFailClosed};
  Rule open{.name = "open", .limit = 100, .refill_per_sec = 10, .failure_policy = FailurePolicy::kFailOpen};
  Rule exact{.name = "exact", .limit = 100, .refill_per_sec = 10, .strict = true};
  return {{"burst", burst}, {"open", open}, {"exact", exact}};
}

// Starts a check; the result appears once the engine answers (inline or after a Reply).
struct Pending {
  std::optional<CheckResult> result;
  void Start(LeasedCore& core, std::string_view key, std::string_view rule, std::uint32_t cost = 1) {
    core.Check({key, rule, cost}, [this](const CheckResult& r) { result = r; });
  }
};

CheckResult CheckNow(LeasedCore& core, std::string_view key, std::string_view rule, std::uint32_t cost = 1) {
  Pending p;
  p.Start(core, key, rule, cost);
  EXPECT_TRUE(p.result.has_value()) << "expected an inline answer";
  return p.result.value_or(CheckResult{});
}

TEST(LeasedCore, FirstCheckWaitsForOneRefillThenHitsStayLocal) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());

  Pending first;
  first.Start(core, "u1", "burst");
  EXPECT_FALSE(first.result.has_value());  // parked until Redis answers
  ASSERT_EQ(redis.Pending(), 1u);
  EXPECT_EQ(redis.Reply(Granted()), 10u);  // asked for one chunk
  ASSERT_TRUE(first.result.has_value());
  EXPECT_TRUE(first.result->allowed);
  EXPECT_EQ(first.result->remaining, 9u);

  for (int left = 8; left >= 3; --left) {  // 6 hits, all local
    const auto r = CheckNow(core, "u1", "burst");
    EXPECT_TRUE(r.allowed);
    EXPECT_EQ(r.remaining, static_cast<std::uint32_t>(left));
  }
  EXPECT_EQ(redis.Total(), 1);

  EXPECT_TRUE(CheckNow(core, "u1", "burst").allowed);  // 2 left: below 30% of the chunk
  EXPECT_EQ(redis.Total(), 2);                         // so a refill started early, nobody waits
  EXPECT_EQ(redis.Pending(), 1u);
}

TEST(LeasedCore, ConcurrentMissesShareOneRefillAndAllGetItsAnswer) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());

  std::atomic<int> denied{0}, answered{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 50; ++i) {
        core.Check({"hot", "burst", 1}, [&](const CheckResult& r) {
          if (!r.allowed && r.reason == Reason::kRateLimited && r.retry_after_ms == 250) ++denied;
          ++answered;
        });
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(redis.Total(), 1);  // 400 misses, one refill
  EXPECT_EQ(answered.load(), 0);

  redis.Reply(Refused(0, 250));
  EXPECT_EQ(answered.load(), 400);
  EXPECT_EQ(denied.load(), 400);

  const auto r = CheckNow(core, "hot", "burst");  // Redis said no: answered locally for a while
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_GT(r.retry_after_ms, 0u);
  EXPECT_EQ(redis.Total(), 1);
}

TEST(LeasedCore, ShortRedisHandsOverExactlyWhatIsLeft) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());

  Pending p;
  p.Start(core, "u2", "burst");
  redis.Reply(Refused(4, 600));               // a chunk of 10 is too much, 4 are left
  EXPECT_EQ(redis.PendingRequested(), 4u);    // so the engine asks for exactly 4
  redis.Reply(Granted());
  ASSERT_TRUE(p.result.has_value());
  EXPECT_TRUE(p.result->allowed);

  for (int i = 0; i < 3; ++i) EXPECT_TRUE(CheckNow(core, "u2", "burst").allowed);
  EXPECT_EQ(redis.Total(), 2);  // Redis is known to be out: no early refill
  const auto r = CheckNow(core, "u2", "burst");
  EXPECT_FALSE(r.allowed);
  EXPECT_GT(r.retry_after_ms, 0u);
  EXPECT_LE(r.retry_after_ms, 600u);
  EXPECT_EQ(redis.Total(), 2);
}

TEST(LeasedCore, UsedUpFullChunkRefillsAgainForTheRest) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());

  std::vector<Pending> checks(15);
  for (auto& p : checks) p.Start(core, "u3", "burst");
  EXPECT_EQ(redis.Total(), 1);
  redis.Reply(Granted());  // 10 tokens for 15 waiters
  int allowed = 0;
  for (auto& p : checks) allowed += p.result.has_value() && p.result->allowed;
  EXPECT_EQ(allowed, 10);
  ASSERT_EQ(redis.Pending(), 1u);  // the other 5 wait for the next chunk, not a denial
  redis.Reply(Granted());
  allowed = 0;
  for (auto& p : checks) allowed += p.result.has_value() && p.result->allowed;
  EXPECT_EQ(allowed, 15);
}

TEST(LeasedCore, RedisDownAppliesTheRulesFailurePolicy) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());

  Pending closed, open;
  closed.Start(core, "u4", "burst");
  open.Start(core, "u4", "open");
  redis.Reply(Down());
  redis.Reply(Down());
  ASSERT_TRUE(closed.result && open.result);
  EXPECT_FALSE(closed.result->allowed);
  EXPECT_EQ(closed.result->reason, Reason::kUnavailable);
  EXPECT_TRUE(open.result->allowed);
}

TEST(LeasedCore, StrictRulesAskRedisOnEveryCheck) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());
  for (int i = 0; i < 3; ++i) {
    Pending p;
    p.Start(core, "u5", "exact", 2);
    EXPECT_EQ(redis.PendingRequested(), 2u);  // the check's own cost, not a chunk
    redis.Reply(Granted());
    EXPECT_TRUE(p.result.has_value());
  }
  EXPECT_EQ(redis.Total(), 3);
}

TEST(LeasedCore, UnknownRuleAndImpossibleCostNeverReachRedis) {
  ManualBackend redis;
  LeasedCore core(redis, Rules());
  EXPECT_EQ(CheckNow(core, "u6", "nope").reason, Reason::kUnknownRule);
  const auto r = CheckNow(core, "u6", "burst", 101);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.retry_after_ms, 0u);  // waiting would not help
  EXPECT_EQ(redis.Total(), 0);
}

}  // namespace
}  // namespace ratelimiter
