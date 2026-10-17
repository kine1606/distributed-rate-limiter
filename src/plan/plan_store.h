#pragma once

#include <functional>
#include <string_view>
#include <utility>

#include "plan/plan.h"
#include "redis/keys.h"
#include "redis/redis_backend.h"

namespace ratelimiter {

// Reads a customer's plan from Redis on every call: one extra round trip per check, which is fine
// for the strict baseline. Week 4 puts a PlanCache in front of it.
class PlanStore {
 public:
  explicit PlanStore(RedisBackend& redis) : redis_(redis) {}  // `redis` must outlive this store

  // `done` gets the plan (Free if the customer has none), or nullptr if Redis could not be asked.
  void Get(std::string_view customer, std::function<void(const Plan*)> done) {
    redis_.HGetAll(PlanKey(customer), [done = std::move(done)](const HashResult& h) {
      if (h.redis_error) {
        done(nullptr);
        return;
      }
      const Plan plan = ParsePlan(h.fields);
      done(&plan);
    });
  }

 private:
  RedisBackend& redis_;
};

}  // namespace ratelimiter
