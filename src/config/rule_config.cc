#include "config/rule_config.h"

#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace ratelimiter {
namespace {

Algorithm ParseAlgorithm(const std::string& rule, const std::string& value) {
  if (value == "token_bucket") return Algorithm::kTokenBucket;
  if (value == "sliding_window") return Algorithm::kSlidingWindow;
  if (value == "fixed_window") return Algorithm::kFixedWindow;
  if (value == "quota") return Algorithm::kQuota;
  throw std::runtime_error("rule '" + rule + "': unknown algorithm '" + value + "'");
}

PlanLimit ParseLimitFrom(const std::string& rule, const std::string& value) {
  if (value.empty()) return PlanLimit::kNone;
  if (value == "plan.daily") return PlanLimit::kDaily;
  if (value == "plan.burst") return PlanLimit::kBurst;
  throw std::runtime_error("rule '" + rule + "': unknown limit_from '" + value + "' (plan.daily | plan.burst)");
}

FailurePolicy ParsePolicy(const std::string& rule, const std::string& value) {
  if (value == "fail_open") return FailurePolicy::kFailOpen;
  if (value == "fail_closed") return FailurePolicy::kFailClosed;
  throw std::runtime_error("rule '" + rule + "': unknown failure_policy '" + value + "'");
}

}  // namespace

RuleTable LoadRules(const std::string& path) {
  YAML::Node root = YAML::LoadFile(path);
  const YAML::Node rules = root["rules"];
  if (!rules || !rules.IsMap()) {
    throw std::runtime_error(path + ": missing top-level 'rules' map");
  }

  RuleTable table;
  for (const auto& entry : rules) {
    Rule rule;
    rule.name = entry.first.as<std::string>();
    const YAML::Node& n = entry.second;

    rule.algorithm = ParseAlgorithm(rule.name, n["algorithm"].as<std::string>("token_bucket"));
    rule.limit = n["limit"].as<std::uint32_t>(0);
    rule.refill_per_sec = n["refill_per_sec"].as<double>(0.0);
    rule.window_ms = n["window_ms"].as<std::uint32_t>(1000);
    rule.strict = n["strict"].as<bool>(false);
    rule.limit_from = ParseLimitFrom(rule.name, n["limit_from"].as<std::string>(""));
    rule.failure_policy = ParsePolicy(rule.name, n["failure_policy"].as<std::string>("fail_closed"));
    rule.burst_rule = n["burst"].as<std::string>("");
    rule.quota_rule = n["quota"].as<std::string>("");
    const std::string quota_mode = n["quota_mode"].as<std::string>("strict");
    rule.quota_auto = quota_mode == "auto";

    const auto fail = [&rule](const std::string& why) { throw std::runtime_error("rule '" + rule.name + "': " + why); };
    // A customer rule only names its two parts; they are checked once every rule is loaded.
    if (!rule.burst_rule.empty() || !rule.quota_rule.empty()) {
      if (rule.burst_rule.empty() || rule.quota_rule.empty()) fail("a customer rule needs both 'burst' and 'quota'");
      table.emplace(rule.name, std::move(rule));
      continue;
    }
    if (rule.limit_from == PlanLimit::kDaily && rule.algorithm != Algorithm::kQuota) {
      fail("limit_from: plan.daily needs algorithm: quota");
    }
    if (rule.limit_from == PlanLimit::kBurst && rule.algorithm != Algorithm::kTokenBucket) {
      fail("limit_from: plan.burst needs algorithm: token_bucket");
    }
    if (rule.limit_from == PlanLimit::kNone && rule.limit == 0) fail("'limit' must be > 0");
    if (rule.limit_from == PlanLimit::kNone && rule.algorithm == Algorithm::kTokenBucket &&
        rule.refill_per_sec <= 0.0) {
      fail("token_bucket needs refill_per_sec > 0");
    }
    if ((rule.algorithm == Algorithm::kSlidingWindow || rule.algorithm == Algorithm::kFixedWindow) &&
        rule.window_ms == 0) {
      fail("'window_ms' must be > 0");
    }
    // A daily quota is exact by default; quota_mode: auto leases only large plans (CompositeCore).
    if (rule.algorithm == Algorithm::kQuota && !rule.strict) fail("quota rules need strict: true");
    if (quota_mode != "strict" && quota_mode != "auto") fail("quota_mode must be strict or auto");
    if (rule.quota_auto && rule.algorithm != Algorithm::kQuota) fail("quota_mode needs algorithm: quota");
    table.emplace(rule.name, std::move(rule));
  }

  for (const auto& [name, rule] : table) {
    if (rule.quota_rule.empty()) continue;
    const auto part = [&](const std::string& field, const std::string& target, Algorithm algorithm) {
      const auto it = table.find(target);
      if (it == table.end()) throw std::runtime_error("rule '" + name + "': " + field + " rule '" + target + "' not found");
      if (it->second.algorithm != algorithm || !it->second.quota_rule.empty()) {
        throw std::runtime_error("rule '" + name + "': " + field + " rule '" + target + "' must be a " +
                                 (algorithm == Algorithm::kQuota ? "quota" : "token_bucket") + " rule");
      }
    };
    part("burst", rule.burst_rule, Algorithm::kTokenBucket);
    part("quota", rule.quota_rule, Algorithm::kQuota);
  }
  return table;
}

}  // namespace ratelimiter
