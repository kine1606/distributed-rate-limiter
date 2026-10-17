#include "redis/keys.h"

#include <gtest/gtest.h>

namespace ratelimiter {
namespace {

// Every key of one customer must share the hash tag, or a script touching two of them fails with
// CROSSSLOT on the cluster.
TEST(Keys, HashTagIsTheEntityAndSuffixIsTheAlgorithm) {
  EXPECT_EQ(RedisKey({.name = "burst"}, "cust:42"), "{cust:42}:burst:tb");
  EXPECT_EQ(RedisKey({.name = "w", .algorithm = Algorithm::kSlidingWindow}, "cust:42"), "{cust:42}:w:sw");
  EXPECT_EQ(RedisKey({.name = "w", .algorithm = Algorithm::kFixedWindow}, "cust:42"), "{cust:42}:w:fw");
  EXPECT_EQ(RedisKey({.name = "daily_quota", .algorithm = Algorithm::kQuota}, "cust:42"), "{cust:42}:daily_quota:q");
  EXPECT_EQ(PlanKey("cust:42"), "{cust:42}:plan");
}

}  // namespace
}  // namespace ratelimiter
