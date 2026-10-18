#include <gtest/gtest.h>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "lease/lease_table.h"

namespace ratelimiter {
namespace {

const Rule kRule{.name = "r", .limit = 100, .refill_per_sec = 10};
const Rule kOther{.name = "other", .limit = 100, .refill_per_sec = 10};

TEST(Lease, ConcurrentTakesNeverOverdraw) {
  Lease lease(kRule, "k");
  lease.tokens = 1000;
  std::atomic<int> taken{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 500; ++i) {
        if (TryTake(lease, 1) >= 0) ++taken;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(taken.load(), 1000);  // 4,000 attempts, 1,000 tokens: exactly 1,000
  EXPECT_EQ(lease.tokens.load(), 0);
}

TEST(Lease, TakeLargerThanTokensTakesNothing) {
  Lease lease(kRule, "k");
  lease.tokens = 3;
  EXPECT_EQ(TryTake(lease, 5), -1);
  EXPECT_EQ(lease.tokens.load(), 3);
  EXPECT_EQ(TryTake(lease, 3), 0);
}

TEST(LeaseTable, RacingFirstLookupsOfOneKeyGetOneLease) {
  LeaseTable table(1024);
  std::vector<Lease*> seen(8);
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] { seen[t] = table.FindOrCreate(kRule, "user:1"); });
  }
  for (auto& t : threads) t.join();
  for (Lease* l : seen) EXPECT_EQ(l, seen[0]);
  ASSERT_NE(seen[0], nullptr);
  EXPECT_EQ(seen[0]->key, "user:1");
}

TEST(LeaseTable, RuleAndKeyTogetherIdentifyALease) {
  LeaseTable table(1024);
  Lease* a = table.FindOrCreate(kRule, "user:1");
  EXPECT_EQ(table.FindOrCreate(kRule, "user:1"), a);
  EXPECT_NE(table.FindOrCreate(kOther, "user:1"), a);
  EXPECT_NE(table.FindOrCreate(kRule, "user:2"), a);
}

TEST(LeaseTable, FullTableReturnsNullAndKeepsExistingLeases) {
  LeaseTable table(64);  // 64 slots, and every key may probe all of them
  std::vector<Lease*> leases;
  for (int i = 0; i < 100; ++i) leases.push_back(table.FindOrCreate(kRule, "k" + std::to_string(i)));

  std::set<Lease*> distinct;
  for (Lease* l : leases) {
    if (l != nullptr) distinct.insert(l);
  }
  EXPECT_EQ(distinct.size(), 64u);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(table.FindOrCreate(kRule, "k" + std::to_string(i)), leases[i]) << i;
  }
}

TEST(ChunkPolicy, StaysBetweenTheMinimumAndTenPercentOfTheLimit) {
  const Rule big{.name = "big", .limit = 1000, .refill_per_sec = 1};
  EXPECT_EQ(ChunkCap(big), 100u);
  EXPECT_EQ(ChunkMin(big), 8u);
  EXPECT_EQ(NextChunk(100, /*emptied=*/true, /*idle=*/false, big), 100u);  // never above the cap
  EXPECT_EQ(NextChunk(8, false, true, big), 8u);                            // never below the minimum
  const Rule tiny{.name = "tiny", .limit = 5, .refill_per_sec = 1};
  EXPECT_EQ(ChunkCap(tiny), 1u);  // a chunk is at least one token, even when 10% rounds to zero
  EXPECT_EQ(ChunkMin(tiny), 1u);
}

TEST(ChunkPolicy, DoublesWhenTheLeaseRanDryHalvesWhenTokensSatUnused) {
  const Rule r{.name = "r", .limit = 1000, .refill_per_sec = 1};
  EXPECT_EQ(NextChunk(25, true, false, r), 50u);
  EXPECT_EQ(NextChunk(50, false, true, r), 25u);
  EXPECT_EQ(NextChunk(50, true, true, r), 50u);    // a quiet key that bursts: no evidence either way
  EXPECT_EQ(NextChunk(50, false, false, r), 50u);
}

TEST(ChunkPolicy, ANewLeaseStartsAtTheCap) { EXPECT_EQ(Lease(kRule, "k").chunk.load(), ChunkCap(kRule)); }

}  // namespace
}  // namespace ratelimiter
