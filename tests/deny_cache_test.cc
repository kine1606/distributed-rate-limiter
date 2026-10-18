#include "lease/deny_cache.h"

#include <gtest/gtest.h>

#include <string>

namespace ratelimiter {
namespace {

constexpr std::int64_t kMs = 1'000'000;

TEST(DenyCache, DeniesUntilTheResetRedisReturned) {
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 0), 60'000u);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 59'999 * kMs), 1u);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 60'000 * kMs), 0u);  // reset reached: entry gone
  EXPECT_EQ(c.Size(), 0u);
  EXPECT_EQ(c.DeniedFor("cust:2", 7, 0), 0u);  // never denied
}

TEST(DenyCache, AnotherPlanIgnoresAndDropsTheDenial) {  // step B5: an upgrade applies at once
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  EXPECT_EQ(c.DeniedFor("cust:1", 8, kMs), 0u);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, kMs), 0u);  // dropped, not kept for the old plan
}

TEST(DenyCache, ClearOnAPlanChangeMessage) {
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  c.Deny("cust:2", 7, 0, 60'000);
  c.Clear("cust:1");
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 0), 0u);
  EXPECT_EQ(c.DeniedFor("cust:2", 7, 0), 60'000u);
}

TEST(DenyCache, CustomersWhoNeverComeBackAreSweptAway) {
  DenyCache c;
  for (int i = 0; i < 200'000; ++i) c.Deny("cust:" + std::to_string(i), 1, i * kMs, 1);  // each expires 1 ms later
  EXPECT_LT(c.Size(), 200'000u / 2);  // expired entries were dropped as the shards grew
  EXPECT_EQ(c.DeniedFor("cust:199999", 1, 199'999 * kMs), 1u);  // the live one is kept
}

}  // namespace
}  // namespace ratelimiter
