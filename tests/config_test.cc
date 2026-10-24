#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "config/rule_config.h"

namespace {

std::string WriteTemp(const std::string& name, const std::string& content) {
  const std::string path = std::string("/tmp/") + name;
  std::ofstream(path) << content;
  return path;
}

}  // namespace

TEST(RuleConfig, LoadsShippedExample) {
  auto rules = ratelimiter::LoadRules(std::string(RL_SOURCE_DIR) + "/config/rules.yaml");
  ASSERT_EQ(rules.count("api_default"), 1u);
  EXPECT_EQ(rules["api_default"].limit, 1000u);
  EXPECT_FALSE(rules["api_default"].strict);
  EXPECT_EQ(rules["api_default"].failure_policy, ratelimiter::FailurePolicy::kFailOpen);
  ASSERT_EQ(rules.count("login_attempts"), 1u);
  EXPECT_TRUE(rules["login_attempts"].strict);
  EXPECT_EQ(rules["login_attempts"].algorithm, ratelimiter::Algorithm::kSlidingWindow);
  ASSERT_EQ(rules.count("demo_fixed_window"), 1u);
  EXPECT_EQ(rules["demo_fixed_window"].algorithm, ratelimiter::Algorithm::kFixedWindow);
  EXPECT_EQ(rules["demo_fixed_window"].window_ms, 5000u);
  EXPECT_TRUE(rules["daily_quota"].quota_auto);  // large plans leased (Week 4)
  EXPECT_FALSE(rules["burst"].quota_auto);
}

TEST(RuleConfig, QuotaModeIsStrictOrAutoAndOnlyOnQuotaRules) {
  const std::string quota = "rules:\n  q:\n    algorithm: quota\n    limit_from: plan.daily\n    strict: true\n";
  EXPECT_FALSE(ratelimiter::LoadRules(WriteTemp("rl_qm_default.yaml", quota))["q"].quota_auto);
  EXPECT_TRUE(ratelimiter::LoadRules(WriteTemp("rl_qm_auto.yaml", quota + "    quota_mode: auto\n"))["q"].quota_auto);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_qm_bad.yaml", quota + "    quota_mode: lease\n")),
               std::runtime_error);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_qm_tb.yaml", "rules:\n  b:\n    limit: 5\n    refill_per_sec: 1\n"
                                                                 "    quota_mode: auto\n")),
               std::runtime_error);
}

TEST(RuleConfig, RejectsZeroWindow) {
  auto p = WriteTemp("rl_zero_window.yaml",
                     "rules:\n  bad:\n    algorithm: fixed_window\n    limit: 5\n    window_ms: 0\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, RejectsZeroLimit) {
  auto p = WriteTemp("rl_zero_limit.yaml",
                     "rules:\n  bad:\n    algorithm: token_bucket\n    limit: 0\n    refill_per_sec: 1\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, LeaseChunkPctDefaultsToTenAndStaysWithinOneToHundred) {
  const std::string tb = "rules:\n  b:\n    limit: 1000\n    refill_per_sec: 1\n";
  EXPECT_EQ(ratelimiter::LoadRules(WriteTemp("rl_pct_default.yaml", tb)).at("b").lease_chunk_pct, 10u);
  EXPECT_EQ(ratelimiter::LoadRules(WriteTemp("rl_pct_5.yaml", tb + "    lease_chunk_pct: 5\n")).at("b").lease_chunk_pct, 5u);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_pct_0.yaml", tb + "    lease_chunk_pct: 0\n")), std::runtime_error);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_pct_101.yaml", tb + "    lease_chunk_pct: 101\n")), std::runtime_error);
}

TEST(RuleConfig, RejectsAnUnknownFailurePolicyAndABucketThatNeverRefills) {
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_bad_policy.yaml",
                                                "rules:\n  b:\n    limit: 5\n    refill_per_sec: 1\n    failure_policy: maybe\n")),
               std::runtime_error);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_no_refill.yaml", "rules:\n  b:\n    limit: 5\n    refill_per_sec: 0\n")),
               std::runtime_error);
}

TEST(RuleConfig, RejectsUnknownAlgorithm) {
  auto p = WriteTemp("rl_bad_algo.yaml", "rules:\n  bad:\n    algorithm: leaky\n    limit: 5\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, RejectsMissingRulesMap) {
  auto p = WriteTemp("rl_no_rules.yaml", "other: 1\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, LoadsPlanRules) {
  auto rules = ratelimiter::LoadRules(std::string(RL_SOURCE_DIR) + "/config/rules.yaml");
  ASSERT_EQ(rules.count("daily_quota"), 1u);
  EXPECT_EQ(rules["daily_quota"].algorithm, ratelimiter::Algorithm::kQuota);
  EXPECT_EQ(rules["daily_quota"].limit_from, ratelimiter::PlanLimit::kDaily);
  EXPECT_TRUE(rules["daily_quota"].strict);
  ASSERT_EQ(rules.count("burst"), 1u);
  EXPECT_EQ(rules["burst"].limit_from, ratelimiter::PlanLimit::kBurst);
}

TEST(RuleConfig, LoadsTheCustomerRule) {
  auto rules = ratelimiter::LoadRules(std::string(RL_SOURCE_DIR) + "/config/rules.yaml");
  ASSERT_EQ(rules.count("customer"), 1u);
  EXPECT_EQ(rules["customer"].burst_rule, "burst");
  EXPECT_EQ(rules["customer"].quota_rule, "daily_quota");
}

TEST(RuleConfig, RejectsBrokenCustomerRules) {
  const std::string parts =
      "  tb:\n    limit: 5\n    refill_per_sec: 1\n  q:\n    algorithm: quota\n    limit: 9\n    strict: true\n";
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_c_half.yaml", "rules:\n" + parts + "  c:\n    burst: tb\n")),
               std::runtime_error);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_c_missing.yaml",
                                                "rules:\n" + parts + "  c:\n    burst: tb\n    quota: nope\n")),
               std::runtime_error);
  EXPECT_THROW(ratelimiter::LoadRules(WriteTemp("rl_c_swapped.yaml",
                                                "rules:\n" + parts + "  c:\n    burst: q\n    quota: tb\n")),
               std::runtime_error);
  EXPECT_EQ(ratelimiter::LoadRules(WriteTemp("rl_c_ok.yaml", "rules:\n" + parts + "  c:\n    burst: tb\n    quota: q\n"))
                .at("c").quota_rule,
            "q");
}

TEST(RuleConfig, QuotaWithFixedLimitIsAllowed) {
  auto p = WriteTemp("rl_fixed_quota.yaml", "rules:\n  q:\n    algorithm: quota\n    limit: 50\n    strict: true\n");
  EXPECT_EQ(ratelimiter::LoadRules(p)["q"].limit, 50u);
}

TEST(RuleConfig, RejectsLeasedQuota) {
  auto p = WriteTemp("rl_leased_quota.yaml", "rules:\n  q:\n    algorithm: quota\n    limit_from: plan.daily\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, RejectsLimitFromThatDoesNotFitTheAlgorithm) {
  auto daily = WriteTemp("rl_daily_tb.yaml",
                         "rules:\n  b:\n    algorithm: token_bucket\n    limit_from: plan.daily\n    strict: true\n");
  EXPECT_THROW(ratelimiter::LoadRules(daily), std::runtime_error);
  auto burst = WriteTemp("rl_burst_quota.yaml",
                         "rules:\n  b:\n    algorithm: quota\n    limit_from: plan.burst\n    strict: true\n");
  EXPECT_THROW(ratelimiter::LoadRules(burst), std::runtime_error);
  auto unknown = WriteTemp("rl_unknown_from.yaml",
                           "rules:\n  b:\n    algorithm: quota\n    limit_from: plan.weekly\n    strict: true\n");
  EXPECT_THROW(ratelimiter::LoadRules(unknown), std::runtime_error);
}
