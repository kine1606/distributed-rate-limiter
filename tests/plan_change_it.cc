// Steps B5 and B9 end to end: tools/rlctl rewrites a plan and publishes it; two limiter instances
// (each a PlanStore with a long TTL, a DenyCache, a PlanListener and a CompositeCore, sharing one
// cluster backend) must apply it within one second. Skipped, not failed, when the cluster or
// redis-cli is missing (seed: RL_TEST_CLUSTER, see it_cluster.h).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "config/rule_config.h"
#include "core/composite_core.h"
#include "it_cluster.h"
#include "redis/keys.h"
#include "redis/plan_listener.h"
#include "redis/redispp_backend.h"

namespace ratelimiter {
namespace {

using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

struct Instance {
  Instance(RedisBackend& redis, const RuleTable& rules)
      : plans(redis, 60s),  // far longer than the test: only the message can apply a change in time
        strict(redis, plans, rules),
        core(std::make_shared<StrictCore>(redis, plans, rules), strict, plans, deny, rules),
        listener(ClusterSeed().first, ClusterSeed().second,
                 [this](std::string_view c) {
                   plans.Invalidate(c);
                   deny.Clear(c);
                 },
                 [this] {
                   plans.InvalidateAll();
                   subscribed = true;
                 }) {}

  CheckResult Check(const std::string& customer) {
    std::promise<CheckResult> p;
    core.Check({customer, "customer", 1}, [&p](const CheckResult& r) { p.set_value(r); });
    auto f = p.get_future();
    EXPECT_EQ(f.wait_for(2s), std::future_status::ready);
    return f.get();
  }
  Plan GetPlan(const std::string& customer) {
    std::promise<Plan> p;
    plans.Get(customer, [&p](const Plan* plan) { p.set_value(plan ? *plan : Plan{}); });
    auto f = p.get_future();
    EXPECT_EQ(f.wait_for(2s), std::future_status::ready);
    return f.get();
  }

  PlanStore plans;
  DenyCache deny;
  StrictCore strict;
  CompositeCore core;
  std::atomic<bool> subscribed{false};
  PlanListener listener;  // last: stopped first
};

class PlanChangeIT : public ::testing::Test {
 protected:
  void SetUp() override {
    admin_ = ConnectAdmin();
    const auto [host, port] = ClusterSeed();
    if (!admin_) GTEST_SKIP() << "Redis Cluster not reachable at " << host << ":" << port;
    if (std::system("command -v redis-cli >/dev/null") != 0) GTEST_SKIP() << "tools/rlctl needs redis-cli";
    const std::string src = RL_SOURCE_DIR;
    rules_ = LoadRules(src + "/config/rules.yaml");
    backend_ = std::make_unique<RedisPPBackend>(host, port, src + "/lua");
    for (auto& i : instances_) i = std::make_unique<Instance>(*backend_, rules_);
    const auto deadline = Steady::now() + 5s;
    for (auto& i : instances_) {
      while (!i->subscribed && Steady::now() < deadline) std::this_thread::sleep_for(5ms);
      ASSERT_TRUE(i->subscribed) << "no plan-changes subscription";
    }
  }

  // Runs tools/rlctl against the test cluster; true on success.
  bool Rlctl(const std::string& args) {
    const auto [host, port] = ClusterSeed();
    const std::string cmd = "REDIS_HOST=" + host + " REDIS_PORT=" + std::to_string(port) + " " + RL_SOURCE_DIR +
                            "/tools/rlctl " + args + " >/dev/null";
    return std::system(cmd.c_str()) == 0;
  }

  // A new customer on Free with nothing used, created through rlctl.
  void Create(const std::string& customer) {
    for (const char* rule : {"daily_quota", "burst"}) admin_->del(RedisKey(rules_.at(rule), customer));
    admin_->del(PlanKey(customer));
    ASSERT_TRUE(Rlctl("create " + customer));
  }

  // The customer's daily counter at `used`, dated `days_ago` days before today by the script's clock.
  void SetUsedToday(const std::string& customer, int used, int days_ago = 0) {
    const std::string key = RedisKey(rules_.at("daily_quota"), customer);
    const std::vector<std::pair<std::string, std::string>> state{
        {"day", std::to_string(RedisDay(key) - days_ago)}, {"used", std::to_string(used)}};
    admin_->hset(key, state.begin(), state.end());
  }
  long long RedisDay(const std::string& key) {  // the UTC day of the master that owns `key`
    return std::stoll(admin_->redis(key, false).command<std::vector<std::string>>("TIME").at(0)) / 86400;
  }

  // ms from `start` until every instance holds a plan with `daily`, or -1 after a second.
  long long MsUntilEveryInstanceSees(const std::string& customer, std::uint32_t daily, Steady::time_point start) {
    for (;;) {
      bool all = true;
      for (auto& i : instances_) all = all && i->GetPlan(customer).daily == daily;
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - start).count();
      if (all) return ms;
      if (ms > 1000) return -1;
      std::this_thread::sleep_for(2ms);
    }
  }

  std::unique_ptr<sw::redis::RedisCluster> admin_;
  RuleTable rules_;
  std::unique_ptr<RedisPPBackend> backend_;
  std::unique_ptr<Instance> instances_[2];  // after the backend: destroyed first
};

// Step B5: Free customer out of quota and denied locally on both instances; after rlctl upgrade
// the next request passes on both, and today's 200 carry over (200 of 2,000 used).
TEST_F(PlanChangeIT, UpgradeReachesEveryInstanceWithinOneSecond) {
  const std::string c = "cust:it-b5";
  Create(c);
  SetUsedToday(c, 200);
  for (auto& i : instances_) {
    const auto r = i->Check(c);
    EXPECT_EQ(r.reason, Reason::kQuotaExhausted);
    EXPECT_EQ(r.limit, 200u);
    EXPECT_GT(i->deny.DeniedFor(c, QuotaStamp(i->GetPlan(c)), SteadyNowNs()), 0u);  // denied locally
  }
  std::this_thread::sleep_for(500ms);  // the listeners sat idle through a few socket timeouts

  const auto start = Steady::now();
  ASSERT_TRUE(Rlctl("upgrade " + c + " --plan pay"));
  const long long ms = MsUntilEveryInstanceSees(c, 2000, start);
  ASSERT_GE(ms, 0) << "upgrade not seen by every instance within one second";
  std::printf("[ measured ] upgrade reached 2 instances %lld ms after rlctl started\n", ms);
  RecordProperty("upgrade_ms", static_cast<int>(ms));

  const auto a = instances_[0]->Check(c);
  const auto b = instances_[1]->Check(c);
  EXPECT_TRUE(a.allowed);
  EXPECT_TRUE(b.allowed);
  EXPECT_EQ(a.limit, 2000u);
  EXPECT_EQ(a.remaining, 1799u);  // usage carried over
  EXPECT_EQ(b.remaining, 1798u);
}

// Evaluation test 2, the reset: yesterday's counter, full, is reset exactly once although both
// instances meet it at the same moment, because the script that resets it runs atomically on the
// master by the master's clock; then the day counts on from there on both.
TEST_F(PlanChangeIT, YesterdaysFullCounterResetsOnceForEveryInstance) {
  const std::string c = "cust:it-reset";
  Create(c);
  SetUsedToday(c, 200, 1);  // out of quota, but yesterday
  auto a = std::async(std::launch::async, [&] { return instances_[0]->Check(c); });
  auto b = std::async(std::launch::async, [&] { return instances_[1]->Check(c); });
  const CheckResult ra = a.get(), rb = b.get();
  EXPECT_TRUE(ra.allowed);
  EXPECT_TRUE(rb.allowed);
  EXPECT_EQ(std::min(ra.remaining, rb.remaining), 198u);  // one reset: 199 and 198, never 199 twice
  EXPECT_EQ(std::max(ra.remaining, rb.remaining), 199u);
  std::this_thread::sleep_for(1100ms);  // Free's burst: 2 tokens, 2 a second
  EXPECT_EQ(instances_[1]->Check(c).remaining, 197u);
  EXPECT_EQ(instances_[0]->Check(c).remaining, 196u);
  const std::string key = RedisKey(rules_.at("daily_quota"), c);
  EXPECT_EQ(admin_->hget(key, "day").value_or(""), std::to_string(RedisDay(key)));
  EXPECT_EQ(admin_->hget(key, "used").value_or(""), "4");
}

// Phase 2, P1 gate: two merchants with their own plan catalogues share the instances; the same
// customer id is a different customer under each; a boost, then a custom limit, set with rlctl reach
// every instance within a second, even past a denial each instance holds locally.
TEST_F(PlanChangeIT, MerchantsCatalogsBoostsAndCustomLimitsReachEveryInstance) {
  for (const std::string m : {"acme", "globex"}) {
    const std::string c = m + "|u1";
    for (const char* rule : {"daily_quota", "burst"}) admin_->del(RedisKey(rules_.at(rule), c));
    admin_->del(PlanKey(c));
  }
  ASSERT_TRUE(Rlctl("--merchant acme plan-define basic daily 2 burst 10 refill 10 overage hard extra 0 failure closed"));
  ASSERT_TRUE(Rlctl("--merchant globex plan-define basic daily 3 burst 10 refill 10 overage hard extra 0 failure closed"));
  ASSERT_TRUE(Rlctl("--merchant acme create u1 --plan basic"));
  ASSERT_TRUE(Rlctl("--merchant globex create u1 --plan basic"));
  const std::string acme = "acme|u1", globex = "globex|u1";  // as the service names them

  EXPECT_TRUE(instances_[0]->Check(acme).allowed);
  EXPECT_TRUE(instances_[1]->Check(acme).allowed);
  for (auto& i : instances_) EXPECT_EQ(i->Check(acme).reason, Reason::kQuotaExhausted);  // denied locally now
  for (int n = 0; n < 3; ++n) EXPECT_TRUE(instances_[n % 2]->Check(globex).allowed) << "globex call " << n;
  EXPECT_FALSE(instances_[0]->Check(globex).allowed);

  const auto until_every = [this](const std::string& c, auto&& seen) {
    const auto start = Steady::now();
    for (;;) {
      bool all = true;
      for (auto& i : instances_) all = all && seen(i->GetPlan(c));
      if (all) return true;
      if (Steady::now() - start > 1s) return false;
      std::this_thread::sleep_for(2ms);
    }
  };
  ASSERT_TRUE(Rlctl("--merchant acme boost u1 2 10"));  // +2 for ten minutes
  ASSERT_TRUE(until_every(acme, [](const Plan& p) { return p.boost == 2; })) << "boost not seen within 1 s";
  const auto a = instances_[0]->Check(acme);
  EXPECT_TRUE(a.allowed);
  EXPECT_EQ(a.limit, 4u);  // 2 + the boost
  EXPECT_TRUE(instances_[1]->Check(acme).allowed);
  EXPECT_FALSE(instances_[0]->Check(acme).allowed);

  ASSERT_TRUE(Rlctl("--merchant acme set u1 daily 100"));
  ASSERT_TRUE(until_every(acme, [](const Plan& p) { return p.daily == 100; })) << "custom limit not seen within 1 s";
  const auto b = instances_[1]->Check(acme);
  EXPECT_TRUE(b.allowed);
  EXPECT_EQ(b.limit, 102u);
  EXPECT_FALSE(instances_[0]->Check(globex).allowed);  // globex's customer untouched
}

// Step B9: suspended on every instance within a second; restored with today's usage intact.
TEST_F(PlanChangeIT, SuspendAndRestoreReachEveryInstance) {
  const std::string c = "cust:it-b9";
  Create(c);
  ASSERT_TRUE(Rlctl("upgrade " + c + " --plan pay"));
  ASSERT_GE(MsUntilEveryInstanceSees(c, 2000, Steady::now()), 0);
  for (auto& i : instances_) EXPECT_TRUE(i->Check(c).allowed);

  ASSERT_TRUE(Rlctl("suspend " + c));
  ASSERT_GE(MsUntilEveryInstanceSees(c, 0, Steady::now()), 0);
  for (auto& i : instances_) EXPECT_EQ(i->Check(c).reason, Reason::kSuspended);

  ASSERT_TRUE(Rlctl("upgrade " + c + " --plan pay"));  // payment settled
  ASSERT_GE(MsUntilEveryInstanceSees(c, 2000, Steady::now()), 0);
  const auto r = instances_[0]->Check(c);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.remaining, 1997u);  // the two checks before the suspension still count
}

// Messages published while the subscription is down are lost, so a listener whose connection was
// killed must come back by itself and call on_resync again (the instance then drops every cached plan),
// and messages after that must arrive again.
TEST(PlanListenerIT, AKilledSubscriptionComesBackAndResyncs) {
  auto admin = ConnectAdmin();
  const auto [host, port] = ClusterSeed();
  if (!admin) GTEST_SKIP() << "Redis Cluster not reachable at " << host << ":" << port;
  if (std::system("command -v redis-cli >/dev/null") != 0) GTEST_SKIP() << "needs redis-cli";
  std::atomic<int> resyncs{0};
  std::mutex mu;
  std::vector<std::string> seen;
  PlanListener listener(host, port,
                        [&](std::string_view c) {
                          std::lock_guard lock(mu);
                          seen.emplace_back(c);
                        },
                        [&] { ++resyncs; });
  const auto wait_for = [](auto done) {
    for (int i = 0; i < 300 && !done(); ++i) std::this_thread::sleep_for(10ms);
    return done();
  };
  ASSERT_TRUE(wait_for([&] { return resyncs.load() == 1; }));
  const std::string kill = "redis-cli -h " + host + " -p " + std::to_string(port) + " CLIENT KILL TYPE pubsub >/dev/null";
  ASSERT_EQ(std::system(kill.c_str()), 0);
  ASSERT_TRUE(wait_for([&] { return resyncs.load() == 2; }));  // back, and told to resync
  admin->publish(std::string(PlanListener::kChannel), "cust:resync");
  EXPECT_TRUE(wait_for([&] {
    std::lock_guard lock(mu);
    return std::find(seen.begin(), seen.end(), "cust:resync") != seen.end();
  }));
}

TEST_F(PlanChangeIT, RlctlRefusesAnUnknownPlanAndASecondCreate) {
  const std::string c = "cust:it-rlctl";
  Create(c);
  EXPECT_FALSE(Rlctl("create " + c));
  EXPECT_FALSE(Rlctl("upgrade " + c + " --plan platinum"));
  EXPECT_FALSE(Rlctl("upgrade " + std::string(129, 'c') + " --plan pay"));
  EXPECT_EQ(admin_->hget(PlanKey(c), "plan").value_or(""), "free");
}

}  // namespace
}  // namespace ratelimiter
