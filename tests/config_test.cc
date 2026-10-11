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

TEST(RuleConfig, RejectsUnknownAlgorithm) {
  auto p = WriteTemp("rl_bad_algo.yaml", "rules:\n  bad:\n    algorithm: leaky\n    limit: 5\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}

TEST(RuleConfig, RejectsMissingRulesMap) {
  auto p = WriteTemp("rl_no_rules.yaml", "other: 1\n");
  EXPECT_THROW(ratelimiter::LoadRules(p), std::runtime_error);
}
