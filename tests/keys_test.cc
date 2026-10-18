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

// quota.lua reads and writes the stored answer in the same call as the counter: same hash tag. The
// rule name keeps two quota rules of one customer from sharing an answer.
TEST(Keys, RequestKeySharesTheCustomersHashTagAndNamesTheRule) {
  const Rule quota{.name = "daily_quota", .algorithm = Algorithm::kQuota};
  const Rule monthly{.name = "monthly", .algorithm = Algorithm::kQuota};
  EXPECT_EQ(RequestKey(quota, "cust:42", "r-1"), "{cust:42}:daily_quota:req:r-1");
  EXPECT_NE(RequestKey(quota, "cust:42", "r-1"), RequestKey(monthly, "cust:42", "r-1"));
  const auto tag = [](const std::string& k) { return k.substr(0, k.find('}') + 1); };
  EXPECT_EQ(tag(RequestKey(quota, "cust:42", "r-1")), tag(RedisKey(quota, "cust:42")));
}

}  // namespace
}  // namespace ratelimiter
