#pragma once

#include "ratelimiter/core.h"

namespace ratelimiter {

// Allows everything, inline, with no Redis: the `allow_all` engine, which measures what gRPC alone
// costs, the baseline every benchmark of the real engines is compared with (bench/README.md).
class AllowAllCore final : public RateLimiterCore {
 public:
  void Check(const CheckRequest&, CheckCallback done) override { done(CheckResult{.allowed = true}); }
};

}  // namespace ratelimiter
