#pragma once
// Transport-independent domain types. Nothing in include/ may depend on gRPC or protobuf
// (see AGENTS.md, "Strict Layer Decoupling").

#include <cstdint>
#include <functional>
#include <string_view>

namespace ratelimiter {

struct Rule;

// What a merchant's calls share (Phase 2, P3; docs/PRODUCT_PLAN.md section 4). Each is a token
// bucket keyed by the merchant, or nullptr when not set:
//   checks    the checks a second the platform takes from the merchant, so its flood cannot slow
//             the others; every check takes one token, whatever its rule
//   capacity  the share of the merchant's backend capacity every plan may use
//   reserve   the rest of that capacity, only for plans with a fee: what paying customers keep
//             under a flood of Free calls
// Built when the merchants are loaded (src/server/merchants.h) and never freed, like the rules.
struct MerchantLimits {
  std::string_view merchant;
  const Rule* checks{nullptr};
  const Rule* capacity{nullptr};
  const Rule* reserve{nullptr};
};

struct CheckRequest {
  std::string_view key;   // views are valid only for the duration of the Check() call
  std::string_view rule;
  std::uint32_t cost{1};
  std::string_view request_id{};  // quota rules count one id once (5 min); empty = no deduplication
  const MerchantLimits* merchant{nullptr};  // tenancy on: the caller's merchant (valid for the process)
};

// Why a check ended the way it did. The gRPC layer maps kUnknownRule to INVALID_ARGUMENT; the
// other values travel in a normal response (proto/rate_limiter.proto, enum Reason).
enum class Reason : std::uint8_t {
  kAllowed,
  kRateLimited,   // bucket empty: retry after retry_after_ms
  kQuotaExhausted,  // daily quota used up: retry after reset_after_ms (00:00 UTC)
  kUnknownRule,   // rule name not in the config
  kUnavailable,   // Redis unreachable and the rule is fail_closed: denied
  kUnverified,    // Redis unreachable and the rule is fail_open: allowed without counting
  kOverage,       // allowed beyond the daily quota under a cap policy: billed as overage
  kSuspended,     // account suspended: denied, waiting does not help
};

struct CheckResult {
  bool allowed{false};
  Reason reason{Reason::kAllowed};
  std::uint32_t remaining{0};
  std::uint32_t retry_after_ms{0};
  std::uint32_t limit{0};
  std::uint32_t reset_after_ms{0};  // daily quota rules only: until 00:00 UTC
  bool replayed{false};  // quota rules: the stored answer of a retried request_id, counted the first time only
};

// Completion callback. The fast path invokes it inline; the slow path (lease refill,
// strict mode) invokes it later from a Redis I/O thread. std::function stays: the Week 7 profile
// found the project's own allocations at about 3% of samples on the customer path and none on the
// lease path, below the run-to-run noise (docs/thesis/evidence/profile_2026-10-24.md).
using CheckCallback = std::function<void(const CheckResult&)>;

}  // namespace ratelimiter
