#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view, ScriptCallback done) override {
    std::lock_guard lock(mu_);
    pending_.push_back({rule.name, std::string(key), requested, std::move(done)});
    ++total_;
  }
  void HGetAll(std::string, HashCallback done) override { done({}); }  // every customer is Free
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    {
      std::lock_guard lock(mu_);
      returned_.push_back(rule.name + "/" + std::string(key) + "/" + std::to_string(tokens));
    }
    done({.allowed = true});
  }
  std::vector<std::string> Returned() {
    std::lock_guard lock(mu_);
    return returned_;
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
  std::vector<std::string> returned_;
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
  Rule plan{.name = "plan", .limit_from = PlanLimit::kBurst};  // not strict, but limits come per customer
  return {{"burst", burst}, {"open", open}, {"exact", exact}, {"plan", plan}};
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
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

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
  EXPECT_EQ(core.GetStats().parked, 1u);   // only the very first check waited
  EXPECT_EQ(core.GetStats().refills, 2u);
}

TEST(LeasedCore, ConcurrentMissesShareOneRefillAndAllGetItsAnswer) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

  std::atomic<int> denied{0}, answered{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 10; ++t) {
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
  EXPECT_EQ(redis.Total(), 1);  // 500 misses, one refill (Week 3 gate)
  EXPECT_EQ(answered.load(), 0);

  redis.Reply(Refused(0, 250));
  EXPECT_EQ(answered.load(), 500);
  EXPECT_EQ(denied.load(), 500);

  const auto r = CheckNow(core, "hot", "burst");  // Redis said no: answered locally for a while
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_GT(r.retry_after_ms, 0u);
  EXPECT_EQ(redis.Total(), 1);
}

TEST(LeasedCore, ShortRedisHandsOverExactlyWhatIsLeft) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

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
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

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
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

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
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());
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
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());
  EXPECT_EQ(CheckNow(core, "u6", "nope").reason, Reason::kUnknownRule);
  const auto r = CheckNow(core, "u6", "burst", 101);
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.retry_after_ms, 0u);  // waiting would not help
  EXPECT_EQ(redis.Total(), 0);
}

TEST(LeasedCore, PlanRulesGoToTheExactPath) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

  Pending p;
  p.Start(core, "cust:1", "plan", 1);
  ASSERT_EQ(redis.Pending(), 1u);
  EXPECT_EQ(redis.PendingRequested(), 1u);  // the check's own cost, not a lease chunk
  redis.Reply(Granted());
  ASSERT_TRUE(p.result.has_value());
  EXPECT_TRUE(p.result->allowed);
  EXPECT_EQ(p.result->limit, 2u);  // Free burst
}

TEST(LeasedCore, ACheckBiggerThanOneChunkAsksRedisDirectly) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());

  Pending big;
  big.Start(core, "u7", "burst", 11);  // chunk cap is 10: a lease could only serve it by holding 11
  ASSERT_EQ(redis.Pending(), 1u);
  EXPECT_EQ(redis.PendingRequested(), 11u);  // exactly its cost, on the shared bucket
  redis.Reply(Granted());
  ASSERT_TRUE(big.result.has_value());
  EXPECT_TRUE(big.result->allowed);
  Pending again;
  again.Start(core, "u7", "burst", 11);
  EXPECT_EQ(redis.Total(), 2);  // every big check is one Redis call
}

std::int64_t g_fake_ns = 0;
std::int64_t FakeNow() { return g_fake_ns; }

TEST(LeasedCore, ChunkHalvesAfterTokensSatUnusedAndDoublesWhenTheLeaseRunsDry) {
  ManualBackend redis;
  PlanStore plans(redis);
  g_fake_ns = 1'000'000'000;
  std::vector<std::pair<std::uint32_t, std::uint32_t>> changes;
  const Rule big{.name = "big", .limit = 1000, .refill_per_sec = 10, .failure_policy = FailurePolicy::kFailClosed};
  LeasedCore core(redis, plans, {{"big", big}}, 1024, &FakeNow,
                  [&](const Lease&, std::uint32_t from, std::uint32_t to) { changes.emplace_back(from, to); });

  Pending first;
  first.Start(core, "q", "big");
  ASSERT_EQ(redis.Reply(Granted()), 100u);  // cap: 10% of 1,000; it ran dry, but cannot grow
  EXPECT_TRUE(changes.empty());
  for (int i = 0; i < 70; ++i) CheckNow(core, "q", "big");  // 29 left: below 30% of 100
  ASSERT_EQ(redis.PendingRequested(), 71u);                   // tops up to one chunk, not 29 + 100

  g_fake_ns += 11'000'000'000;  // that chunk lasted 11 s: its tokens sat unused
  redis.Reply(Granted());
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0], std::make_pair(100u, 50u));

  for (int i = 0; i < 86; ++i) CheckNow(core, "q", "big");  // 100 -> 14: below 30% of 50
  ASSERT_EQ(redis.PendingRequested(), 36u);                   // tops up to the halved chunk
  for (int i = 0; i < 14; ++i) CheckNow(core, "q", "big");   // dry while that refill is out
  Pending waiting;
  waiting.Start(core, "q", "big");
  EXPECT_FALSE(waiting.result.has_value());
  redis.Reply(Granted());
  ASSERT_TRUE(waiting.result.has_value());
  EXPECT_TRUE(waiting.result->allowed);
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[1], std::make_pair(50u, 100u));  // demand outran the chunk: doubled
}

// Week 5 shutdown: unused leased tokens go back to Redis, so a restart does not burn them.
// A full lease table never refuses a check: keys it cannot hold take the exact path.
TEST(LeasedCore, KeysAFullLeaseTableCannotHoldTakeTheExactPath) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules(), 64);  // 64 slots
  std::vector<Pending> checks(100);
  for (int i = 0; i < 100; ++i) checks[static_cast<std::size_t>(i)].Start(core, "k" + std::to_string(i), "burst");
  ASSERT_EQ(redis.Pending(), 100u);
  int chunks = 0, exact = 0;
  while (redis.Pending() > 0) ++(redis.Reply(Granted()) == 10 ? chunks : exact);
  EXPECT_EQ(chunks, 64);  // a lease refill of one chunk each
  EXPECT_EQ(exact, 36);   // one script per check
  for (const auto& c : checks) {
    ASSERT_TRUE(c.result.has_value());
    EXPECT_TRUE(c.result->allowed);
  }
}

// The ablation switches (Week 7): each mechanism off on its own, everything else unchanged.
TEST(LeasedCore, WithoutEarlyRefillTheLeaseRunsDryBeforeAskingAgain) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules(), 1024, &SteadyNowNs, {}, {.early_refill = false});
  Pending first;
  first.Start(core, "u1", "burst");
  redis.Reply(Granted());                                     // 10 tokens, one taken
  for (int i = 0; i < 9; ++i) EXPECT_TRUE(CheckNow(core, "u1", "burst").allowed);  // to 0: no refill
  EXPECT_EQ(redis.Total(), 1);
  Pending dry;
  dry.Start(core, "u1", "burst");
  EXPECT_FALSE(dry.result.has_value());  // waits for the refill an early one would have avoided
  EXPECT_EQ(redis.Total(), 2);
  redis.Reply(Granted());
}

TEST(LeasedCore, WithoutAdaptiveChunkTheChunkStaysAtTheCap) {
  ManualBackend redis;
  PlanStore plans(redis);
  g_fake_ns = 1'000'000'000;
  const Rule big{.name = "big", .limit = 1000, .refill_per_sec = 10, .failure_policy = FailurePolicy::kFailClosed};
  int changes = 0;
  LeasedCore core(redis, plans, {{"big", big}}, 1024, &FakeNow, [&](const Lease&, std::uint32_t, std::uint32_t) { ++changes; },
                  {.adaptive_chunk = false});
  Pending first;
  first.Start(core, "q", "big");
  redis.Reply(Granted());
  for (int i = 0; i < 70; ++i) CheckNow(core, "q", "big");
  g_fake_ns += 11'000'000'000;  // the chunk sat unused for 11 s: adaptive would halve it
  redis.Reply(Granted());
  for (int i = 0; i < 71; ++i) CheckNow(core, "q", "big");  // 100 -> 29: below 30% of 100
  EXPECT_EQ(redis.PendingRequested(), 71u);                   // still topping up to 100
  EXPECT_EQ(changes, 0);
  redis.Reply(Granted());
}

TEST(LeasedCore, ReturnLeasesGivesBackWhatEachLeaseStillHolds) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());
  Pending a, b;
  a.Start(core, "u1", "burst");
  b.Start(core, "u2", "open");
  redis.Reply(Granted());  // 10 tokens each, one taken
  redis.Reply(Granted());
  CheckNow(core, "u1", "burst");  // u1 holds 8, u2 holds 9

  const auto r = core.ReturnLeases(std::chrono::seconds(1));
  EXPECT_EQ(r.leases, 2u);
  EXPECT_EQ(r.tokens, 17u);
  EXPECT_EQ(r.failed, 0u);
  auto returned = redis.Returned();
  std::sort(returned.begin(), returned.end());
  EXPECT_EQ(returned, (std::vector<std::string>{"burst/u1/8", "open/u2/9"}));
  EXPECT_EQ(core.ReturnLeases(std::chrono::seconds(1)).tokens, 0u);  // nothing left to give twice
}

// A refill still in flight at shutdown brings tokens; they must be returned too (code review, Week 5).
TEST(LeasedCore, ReturnLeasesWaitsForARefillInFlight) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());
  Pending first;
  first.Start(core, "u1", "burst");  // its refill of 10 is out
  LeasedCore::Returned r{};
  std::thread shutdown([&] { r = core.ReturnLeases(std::chrono::seconds(2)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  redis.Reply(Granted());  // lands during the shutdown: 10, one taken by the parked check
  shutdown.join();
  EXPECT_EQ(r.tokens, 9u);
  EXPECT_EQ(r.refills_unfinished, 0u);
}

TEST(LeasedCore, ReturnLeasesReportsARefillThatNeverLanded) {
  ManualBackend redis;
  PlanStore plans(redis);
  LeasedCore core(redis, plans, Rules());
  Pending first;
  first.Start(core, "u1", "burst");
  const auto r = core.ReturnLeases(std::chrono::milliseconds(30));
  EXPECT_EQ(r.refills_unfinished, 1u);  // logged, not hidden behind "0 returns failed"
  redis.Reply(Granted());               // let the parked check finish before the core goes
}

}  // namespace
}  // namespace ratelimiter
