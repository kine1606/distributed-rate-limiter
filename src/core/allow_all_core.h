#pragma once

#include "ratelimiter/core.h"

namespace ratelimiter {

// Stage-2 stand-in so the gRPC server runs end to end before the real engines exist.
// Allows everything. Replaced by the Redis-backed engine (Week 2) and the lease engine (Week 3).
class AllowAllCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest& request, CheckCallback done) override {
    CheckResult result;
    result.allowed = true;
    result.remaining = 0;
    result.retry_after_ms = 0;
    result.limit = 0;
    (void)request;
    done(result);  // inline completion: this is the shape the lease fast path will have
  }
};

}  // namespace ratelimiter
