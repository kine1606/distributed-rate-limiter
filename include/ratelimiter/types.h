#pragma once
// Transport-independent domain types. Nothing in include/ may depend on gRPC or protobuf
// (see AGENTS.md, "Strict Layer Decoupling").

#include <cstdint>
#include <functional>
#include <string_view>

namespace ratelimiter {

struct CheckRequest {
  std::string_view key;   // views are valid only for the duration of the Check() call
  std::string_view rule;
  std::uint32_t cost{1};
  std::string_view request_id{};  // quota rules count one id once (5 min); empty = no deduplication
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
};

// Completion callback. The fast path invokes it inline; the slow path (lease refill,
// strict mode) invokes it later from a Redis I/O thread. Replace std::function with a
// pooled, allocation-free callback in the lease stage (Week 3) once profiling justifies it.
using CheckCallback = std::function<void(const CheckResult&)>;

}  // namespace ratelimiter
