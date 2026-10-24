#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "metrics/metrics.h"
#include "ratelimiter/rule.h"
#include "ratelimiter/types.h"

namespace ratelimiter {

// What the gRPC service measures per rule: answers by reason and server-side latency, plus calls
// refused before any engine. Every rule's label set exists before its first check, so recording a
// check is one map lookup by the request's view and a few counter adds: nothing is allocated per
// check. Labels are rule and reason only: a customer id would make one time series per customer.
class CheckMetrics {
 public:
  struct PerRule {
    std::array<Counter, 8> by_reason;  // indexed by Reason
    Histogram latency;
  };

  explicit CheckMetrics(const RuleTable& rules) { AddRules(rules); }

  PerRule* Of(std::string_view rule) const {  // nullptr for a rule not in the config
    const Index& index = *index_.load(std::memory_order_acquire);
    const auto it = index.find(rule);
    return it == index.end() ? nullptr : it->second;
  }

  // After a rule reload: rules new to this process get their label set; the others keep theirs, so
  // their counters go on (a removed rule's stop growing). Checks may read the old index meanwhile,
  // so none is freed: a few hundred bytes per reload.
  void AddRules(const RuleTable& rules) {
    std::lock_guard lock(mu_);
    auto next = indexes_.empty() ? std::make_unique<Index>() : std::make_unique<Index>(*indexes_.back());
    for (const auto& [name, rule] : rules) {
      if (!next->contains(name)) next->emplace(name, &per_rule_.emplace_back());
    }
    index_.store(next.get(), std::memory_order_release);
    indexes_.push_back(std::move(next));
  }

  Counter rejected_overload, rejected_invalid;

  void Render(std::string& out) const {
    std::lock_guard lock(mu_);
    const Index& rules = *indexes_.back();
    static constexpr std::array<const char*, 8> kReason{"ok",          "rate_limited", "quota_exhausted", "unknown_rule",
                                                        "unavailable", "unverified",   "overage",         "suspended"};
    static_assert(static_cast<std::size_t>(Reason::kSuspended) + 1 == kReason.size());
    AppendFamily(out, "rl_checks_total", "counter", "Checks answered, by rule and reason");
    for (const auto& [name, m] : rules) {
      for (std::size_t i = 0; i < kReason.size(); ++i) {
        if (const auto n = m->by_reason[i].Value(); n > 0) {
          AppendSample(out, "rl_checks_total", "rule=\"" + LabelValue(name) + "\",reason=\"" + kReason[i] + "\"",
                       static_cast<double>(n));
        }
      }
    }
    AppendFamily(out, "rl_check_latency_seconds", "histogram", "Server-side check latency, call to answer, by rule");
    for (const auto& [name, m] : rules) m->latency.Render(out, "rl_check_latency_seconds", "rule=\"" + LabelValue(name) + "\"");
    AppendFamily(out, "rl_rejected_total", "counter", "Calls refused before any engine, by reason");
    AppendSample(out, "rl_rejected_total", "reason=\"overload\"", static_cast<double>(rejected_overload.Value()));
    AppendSample(out, "rl_rejected_total", "reason=\"invalid\"", static_cast<double>(rejected_invalid.Value()));
  }

 private:
  using Index = std::unordered_map<std::string, PerRule*, StringHash, std::equal_to<>>;
  mutable std::mutex mu_;  // reloads and Render only
  std::deque<PerRule> per_rule_;  // a deque never moves its elements
  std::vector<std::unique_ptr<Index>> indexes_;  // current last
  std::atomic<const Index*> index_{nullptr};
};

}  // namespace ratelimiter
