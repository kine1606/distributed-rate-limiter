#pragma once

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "metrics/metrics.h"
#include "ratelimiter/rule.h"
#include "ratelimiter/types.h"

namespace ratelimiter {

// What the gRPC service measures per rule: answers by reason and server-side latency, plus calls
// refused before any engine. Every rule's label set exists from the start, so recording a check is
// one map lookup by the request's view and a few counter adds: nothing is allocated per check.
// Labels are rule and reason only: a customer id would make one time series per customer.
class CheckMetrics {
 public:
  struct PerRule {
    std::array<Counter, 8> by_reason;  // indexed by Reason
    Histogram latency;
  };

  explicit CheckMetrics(const RuleTable& rules) {
    for (const auto& [name, rule] : rules) rules_.emplace(name, std::make_unique<PerRule>());
  }

  PerRule* Of(std::string_view rule) {  // nullptr for a rule not in the config
    const auto it = rules_.find(rule);
    return it == rules_.end() ? nullptr : it->second.get();
  }

  Counter rejected_overload, rejected_invalid;

  void Render(std::string& out) const {
    static constexpr std::array<const char*, 8> kReason{"ok",          "rate_limited", "quota_exhausted", "unknown_rule",
                                                        "unavailable", "unverified",   "overage",         "suspended"};
    static_assert(static_cast<std::size_t>(Reason::kSuspended) + 1 == kReason.size());
    AppendFamily(out, "rl_checks_total", "counter", "Checks answered, by rule and reason");
    for (const auto& [name, m] : rules_) {
      for (std::size_t i = 0; i < kReason.size(); ++i) {
        if (const auto n = m->by_reason[i].Value(); n > 0) {
          AppendSample(out, "rl_checks_total", "rule=\"" + LabelValue(name) + "\",reason=\"" + kReason[i] + "\"",
                       static_cast<double>(n));
        }
      }
    }
    AppendFamily(out, "rl_check_latency_seconds", "histogram", "Server-side check latency, call to answer, by rule");
    for (const auto& [name, m] : rules_) m->latency.Render(out, "rl_check_latency_seconds", "rule=\"" + LabelValue(name) + "\"");
    AppendFamily(out, "rl_rejected_total", "counter", "Calls refused before any engine, by reason");
    AppendSample(out, "rl_rejected_total", "reason=\"overload\"", static_cast<double>(rejected_overload.Value()));
    AppendSample(out, "rl_rejected_total", "reason=\"invalid\"", static_cast<double>(rejected_invalid.Value()));
  }

 private:
  std::unordered_map<std::string, std::unique_ptr<PerRule>, StringHash, std::equal_to<>> rules_;
};

}  // namespace ratelimiter
