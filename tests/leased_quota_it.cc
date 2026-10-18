// Week 4 gate, second part: an enterprise customer on leased quota stays within limit + N x C.
// Four limiter instances (each its own LeasedCore, CompositeCore, PlanStore and DenyCache) share one
// cluster backend and one customer of 100,000 a day, the smallest leased plan (C = 100). Checks of
// cost 10 keep the run short. Skipped, not failed, when the cluster is unreachable.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "config/rule_config.h"
#include "core/composite_core.h"
#include "it_cluster.h"
#include "redis/keys.h"
#include "redis/redispp_backend.h"

namespace ratelimiter {
namespace {

struct Instance {
  Instance(RedisBackend& redis, const RuleTable& rules)
      : plans(redis, std::chrono::seconds(5)),
        strict(redis, plans, rules),
        leased(std::make_shared<LeasedCore>(redis, plans, rules)),
        core(leased, strict, plans, deny, rules, &SteadyNowNs, {}, leased.get()) {}

  PlanStore plans;
  DenyCache deny;
  StrictCore strict;
  std::shared_ptr<LeasedCore> leased;
  CompositeCore core;
};

TEST(LeasedQuotaIT, EnterpriseCustomerStaysWithinLimitPlusNTimesC) {
  const auto admin = ConnectAdmin();
  const auto [host, port] = ClusterSeed();
  if (!admin) GTEST_SKIP() << "Redis Cluster not reachable at " << host << ":" << port;
  const std::string src = RL_SOURCE_DIR;
  const RuleTable rules = LoadRules(src + "/config/rules.yaml");
  ASSERT_TRUE(rules.at("daily_quota").quota_auto);
  RedisPPBackend backend(host, port, src + "/lua");

  constexpr int kInstances = 4, kThreads = 8;
  constexpr std::uint32_t kLimit = kLeasedQuotaFrom, kCost = 10;
  const std::uint32_t chunk = ChunkCap(Rule{.name = "q", .algorithm = Algorithm::kQuota, .limit = kLimit});
  const std::string c = "cust:it-ent";
  for (const char* rule : {"daily_quota", "burst"}) admin->del(RedisKey(rules.at(rule), c));
  admin->del(PlanKey(c));
  const std::vector<std::pair<std::string, std::string>> plan{
      {"plan", "enterprise"}, {"daily", std::to_string(kLimit)}, {"burst", "10000000"}, {"refill", "10000000"},
      {"overage", "hard"},    {"extra", "0"}};  // burst far above the run: only the quota limits it
  admin->hset(PlanKey(c), plan.begin(), plan.end());

  std::vector<std::unique_ptr<Instance>> instances;
  for (int i = 0; i < kInstances; ++i) instances.push_back(std::make_unique<Instance>(backend, rules));

  // 20% more demand than the bound allows, spread over the instances.
  const int per_thread = static_cast<int>((kLimit + kInstances * chunk) * 1.2 / kCost / kThreads);
  std::atomic<std::uint64_t> admitted{0}, exhausted{0}, other{0};
  const auto start = std::chrono::steady_clock::now();
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      CompositeCore& core = instances[t % kInstances]->core;
      for (int i = 0; i < per_thread; ++i) {
        std::promise<CheckResult> p;
        core.Check({c, "customer", kCost}, [&p](const CheckResult& r) { p.set_value(r); });
        const CheckResult r = p.get_future().get();
        if (r.allowed && r.reason == Reason::kAllowed) {
          admitted += kCost;
        } else if (!r.allowed && r.reason == Reason::kQuotaExhausted) {
          ++exhausted;
        } else {
          ++other;
        }
      }
    });
  }
  for (auto& t : threads) t.join();
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::uint64_t refills = 0;
  for (auto& i : instances) refills += i->leased->GetStats().refills;
  const std::uint64_t bound = std::uint64_t{kInstances} * chunk;
  std::printf("[ measured ] admitted %llu of %u units (bound %llu to %llu); %llu quota refills for %d checks; %.1f s\n",
              static_cast<unsigned long long>(admitted.load()), kLimit,
              static_cast<unsigned long long>(kLimit - bound), static_cast<unsigned long long>(kLimit + bound),
              static_cast<unsigned long long>(refills), per_thread * kThreads, secs);
  RecordProperty("admitted", static_cast<int>(admitted.load()));
  RecordProperty("quota_refills", static_cast<int>(refills));

  EXPECT_LE(admitted.load(), kLimit + bound);  // the gate
  EXPECT_GE(admitted.load(), kLimit - bound);  // stranded tokens: at most a chunk per instance
  EXPECT_GT(exhausted.load(), 0u);
  EXPECT_EQ(other.load(), 0u);
  EXPECT_LE(std::stoull(admin->hget(RedisKey(rules.at("daily_quota"), c), "used").value_or("0")), kLimit);
  // A refill tops up to the chunk from the 30% mark, so it brings 70 to 100 units: on average over
  // half a chunk. The exact path would have sent one quota call per check (cost 10: 10 units).
  EXPECT_LT(refills * chunk / 2, admitted.load());
}

}  // namespace
}  // namespace ratelimiter
