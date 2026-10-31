#include "lease/deny_cache.h"

#include <gtest/gtest.h>

#include <string>
#include <cstdint>
#include <limits>
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

TEST(DenyCache, ADisabledCacheNeverDenies) {  // the Week 7 ablation switch
  DenyCache c(false);
  c.Deny("cust:1", 7, 0, 60'000);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 1), 0u);
  EXPECT_EQ(c.Size(), 0u);
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


// ---- Week 4 edge cases (2026-11-01) ----

TEST(DenyCache, ADenialThatEndsNowDeniesNothing) {
  DenyCache c;
  c.Deny("cust:1", 7, 5 * kMs, 0);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 5 * kMs), 0u);
  EXPECT_EQ(c.Size(), 0u);
}

TEST(DenyCache, TheTimeLeftRoundsUpSoItIsNeverZeroWhileDenied) {
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 1), 60'000u);              // 1 ns in
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 60'000 * kMs - 1), 1u);    // 1 ns before the reset
}

TEST(DenyCache, TheLongestResetDoesNotOverflow) {
  DenyCache c;
  const std::uint32_t longest = std::numeric_limits<std::uint32_t>::max();  // about 49.7 days
  c.Deny("cust:1", 7, 0, longest);
  EXPECT_EQ(c.DeniedFor("cust:1", 7, 0), longest);
}

TEST(DenyCache, ClearingACustomerItNeverDeniedIsHarmless) {
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  c.Clear("nobody");
  EXPECT_EQ(c.DeniedFor("cust:1", 7, kMs), 59'999u);
}

TEST(DenyCache, CustomersDifferingByOneByteAreKeptApart) {
  DenyCache c;
  c.Deny("cust:1", 7, 0, 60'000);
  for (const char* other : {"cust:10", "cust:", "Cust:1", "cust:1 "}) EXPECT_EQ(c.DeniedFor(other, 7, kMs), 0u) << other;
  EXPECT_GT(c.DeniedFor("cust:1", 7, kMs), 0u);
}

TEST(DenyCache, TenThousandDenialsAreAllKeptUntilTheirReset) {
  DenyCache c;
  for (int i = 0; i < 10'000; ++i) c.Deny("cust:" + std::to_string(i), 7, 0, 60'000);
  EXPECT_EQ(c.Size(), 10'000u);
  int denied = 0;
  for (int i = 0; i < 10'000; ++i) denied += c.DeniedFor("cust:" + std::to_string(i), 7, kMs) > 0;
  EXPECT_EQ(denied, 10'000);
}

}  // namespace
}  // namespace ratelimiter
