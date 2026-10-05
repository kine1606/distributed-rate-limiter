#pragma once

#include "ratelimiter/types.h"

namespace ratelimiter {

// Rate-limiting engine interface. The gRPC layer depends only on this interface
// (dependency injection), so engines can be swapped and unit-tested without a network.
//
// Implementations must be thread-safe and must never block the calling (gRPC) thread:
// anything that needs Redis completes through `done` on another thread.
class RateLimiterCore {
 public:
  virtual ~RateLimiterCore() = default;
  virtual void Check(const CheckRequest& request, CheckCallback done) = 0;
};

}  // namespace ratelimiter
