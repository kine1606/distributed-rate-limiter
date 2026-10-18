// Steps B5 and B9 end to end: tools/rlctl rewrites a plan and publishes it; two limiter instances
// (each a PlanStore with a long TTL, a DenyCache, a PlanListener and a CompositeCore, sharing one
// cluster backend) must apply it within one second. Skipped, not failed, when the cluster or
// redis-cli is missing (seed: RL_TEST_CLUSTER, see it_cluster.h).
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <memory>
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

  void SetUsedToday(const std::string& customer, int used) {
    const std::string key = RedisKey(rules_.at("daily_quota"), customer);
    const auto t = admin_->redis(key, false).command<std::vector<std::string>>("TIME");  // the script's clock
    const std::vector<std::pair<std::string, std::string>> state{
        {"day", std::to_string(std::stoll(t.at(0)) / 86400)}, {"used", std::to_string(used)}};
    admin_->hset(key, state.begin(), state.end());
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
