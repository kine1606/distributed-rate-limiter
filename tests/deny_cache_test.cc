#include "lease/deny_cache.h"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

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

TEST(DenyCache, ALaterDenialReplacesTheEarlierOne) {  // e.g. a cap customer's overage ran out later
  DenyCache c;
  c.Deny("cust:1", 7, 0, 1'000);
  c.Deny("cust:1", 8, 0, 60'000);
  EXPECT_EQ(c.DeniedFor("cust:1", 8, 2'000 * kMs), 58'000u);
  EXPECT_EQ(c.Size(), 1u);
}

TEST(DenyCache, ConcurrentDenyCheckAndClearAreSafe) {  // for TSan: shards are shared by threads
  DenyCache c;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&c, t] {
      for (int i = 0; i < 5'000; ++i) {
        const std::string customer = "cust:" + std::to_string(i % 64);
        if ((i + t) % 3 == 0) c.Deny(customer, 1, i, 60'000);
        if ((i + t) % 3 == 1) c.DeniedFor(customer, 1, i);
        if ((i + t) % 7 == 0) c.Clear(customer);
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_LE(c.Size(), 64u);
}

TEST(DenyCache, CustomersWhoNeverComeBackAreSweptAway) {
  DenyCache c;
  for (int i = 0; i < 200'000; ++i) c.Deny("cust:" + std::to_string(i), 1, i * kMs, 1);  // each expires 1 ms later
  EXPECT_LT(c.Size(), 200'000u / 2);  // expired entries were dropped as the shards grew
  EXPECT_EQ(c.DeniedFor("cust:199999", 1, 199'999 * kMs), 1u);  // the live one is kept
}

}  // namespace
}  // namespace ratelimiter
