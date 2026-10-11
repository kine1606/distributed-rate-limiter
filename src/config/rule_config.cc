#include "config/rule_config.h"

#include <stdexcept>

#include <yaml-cpp/yaml.h>

namespace ratelimiter {
namespace {

Algorithm ParseAlgorithm(const std::string& rule, const std::string& value) {
  if (value == "token_bucket") return Algorithm::kTokenBucket;
  if (value == "sliding_window") return Algorithm::kSlidingWindow;
  if (value == "fixed_window") return Algorithm::kFixedWindow;
  throw std::runtime_error("rule '" + rule + "': unknown algorithm '" + value + "'");
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
    rule.failure_policy = ParsePolicy(rule.name, n["failure_policy"].as<std::string>("fail_closed"));

    if (rule.limit == 0) {
      throw std::runtime_error("rule '" + rule.name + "': 'limit' must be > 0");
    }
    if (rule.algorithm == Algorithm::kTokenBucket && rule.refill_per_sec <= 0.0) {
      throw std::runtime_error("rule '" + rule.name + "': token_bucket needs refill_per_sec > 0");
    }
    if (rule.algorithm != Algorithm::kTokenBucket && rule.window_ms == 0) {
      throw std::runtime_error("rule '" + rule.name + "': 'window_ms' must be > 0");
    }
    table.emplace(rule.name, std::move(rule));
  }
  return table;
}

}  // namespace ratelimiter
