#pragma once
// The only place that knows both the proto types and the domain types (AGENTS.md, layer decoupling).

#include <cstddef>

#include "rate_limiter.pb.h"
#include "ratelimiter/types.h"

namespace ratelimiter {

// Request limits promised in proto/rate_limiter.proto.
inline constexpr std::size_t kMaxIdBytes = 128;  // key and request_id
inline constexpr int kMaxBatch = 1000;

// Why a request is malformed (the INVALID_ARGUMENT message), or nullptr if it is fine.
inline const char* Invalid(const v1::CheckRequest& r) {
  if (r.key().empty() || r.rule().empty()) return "key and rule are required";
  if (r.key().size() > kMaxIdBytes || r.request_id().size() > kMaxIdBytes) {
    return "key and request_id are at most 128 bytes";
  }
  return nullptr;
}

// The views point into `in`, which gRPC keeps alive until the call finishes.
inline CheckRequest ToDomain(const v1::CheckRequest& in) {
  return CheckRequest{in.key(), in.rule(), in.cost() == 0 ? 1u : in.cost(), in.request_id()};
}

inline v1::Reason ToProto(Reason r) {
  switch (r) {
    case Reason::kAllowed: return v1::REASON_OK;
    case Reason::kRateLimited: return v1::REASON_RATE_LIMITED;
    case Reason::kQuotaExhausted: return v1::REASON_QUOTA_EXHAUSTED;
    case Reason::kUnavailable: return v1::REASON_UNAVAILABLE;
    case Reason::kUnverified: return v1::REASON_UNVERIFIED;
    case Reason::kOverage: return v1::REASON_OVERAGE;
    case Reason::kSuspended: return v1::REASON_SUSPENDED;
    case Reason::kUnknownRule: break;  // answered with INVALID_ARGUMENT, never filled
  }
  return v1::REASON_UNSPECIFIED;
}

inline void Fill(const CheckResult& r, v1::CheckResponse* out) {
  out->set_allowed(r.allowed);
  out->set_remaining(r.remaining);
  out->set_retry_after_ms(r.retry_after_ms);
  out->set_limit(r.limit);
  out->set_reason(ToProto(r.reason));
  out->set_reset_after_ms(r.reset_after_ms);
}

}  // namespace ratelimiter
