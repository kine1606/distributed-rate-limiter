#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <limits>
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

// The cap is the knob of the accuracy bound N x C (Chapter 3, 3.5.3), so a rule may set it.
TEST(ChunkPolicy, ARuleMaySetItsCapInPercentOfTheLimit) {
  EXPECT_EQ(ChunkCap(Rule{.name = "r", .limit = 1000, .refill_per_sec = 1, .lease_chunk_pct = 1}), 10u);
  EXPECT_EQ(ChunkCap(Rule{.name = "r", .limit = 1000, .refill_per_sec = 1, .lease_chunk_pct = 5}), 50u);
  EXPECT_EQ(ChunkCap(Rule{.name = "r", .limit = 4'000'000'000u, .refill_per_sec = 1, .lease_chunk_pct = 100}),
            4'000'000'000u);  // no overflow on the way
}

// Leased quota (Week 4): 0.1% of the daily limit, so 4 instances stay within 0.4% of a day.
TEST(ChunkPolicy, ALeasedDailyQuotaTakesChunksOfOneTenthOfAPercent) {
  const Rule enterprise{.name = "q", .algorithm = Algorithm::kQuota, .limit = 1'000'000};
  EXPECT_EQ(ChunkCap(enterprise), 1000u);
  EXPECT_EQ(ChunkMin(enterprise), 8u);
  EXPECT_EQ(ChunkCap(Rule{.name = "q", .algorithm = Algorithm::kQuota, .limit = kLeasedQuotaFrom}), 100u);
}

TEST(ChunkPolicy, DoublesWhenTheLeaseRanDryHalvesWhenTokensSatUnused) {
  const Rule r{.name = "r", .limit = 1000, .refill_per_sec = 1};
  EXPECT_EQ(NextChunk(25, true, false, r), 50u);
  EXPECT_EQ(NextChunk(50, false, true, r), 25u);
  EXPECT_EQ(NextChunk(50, true, true, r), 50u);    // a quiet key that bursts: no evidence either way
  EXPECT_EQ(NextChunk(50, false, false, r), 50u);
}

TEST(ChunkPolicy, ANewLeaseStartsAtTheCap) { EXPECT_EQ(Lease(kRule, "k").chunk.load(), ChunkCap(kRule)); }


// ---- Week 3 edge cases (2026-11-01) ----

TEST(Lease, TakingExactlyEveryTokenLeavesZeroAndTheNextTakeFails) {
  Lease lease(kRule, "k");
  lease.tokens = 5;
  EXPECT_EQ(TryTake(lease, 5), 0);
  EXPECT_EQ(TryTake(lease, 1), -1);
  EXPECT_EQ(lease.tokens.load(), 0);
}

TEST(Lease, ConcurrentTakesOfMixedCostsNeverOverdraw) {
  Lease lease(kRule, "k");
  lease.tokens = 10'000;
  std::atomic<std::int64_t> taken{0};
  std::vector<std::thread> threads;
  for (const std::uint32_t cost : {1u, 2u, 3u, 7u, 1u, 2u, 3u, 7u}) {
    threads.emplace_back([&, cost] {
      for (int i = 0; i < 2'000; ++i) {
        if (TryTake(lease, cost) >= 0) taken += cost;
      }
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_GE(lease.tokens.load(), 0);
  EXPECT_EQ(taken.load() + lease.tokens.load(), 10'000);  // every token either taken once or still there
}

TEST(Lease, TheLargestCostFailsCleanly) {
  Lease lease(kRule, "k");
  lease.tokens = 5;
  EXPECT_EQ(TryTake(lease, std::numeric_limits<std::uint32_t>::max()), -1);
  EXPECT_EQ(lease.tokens.load(), 5);
}

TEST(LeaseTable, ManyKeysEachGetTheirOwnLeaseAndKeepIt) {
  LeaseTable table(1 << 16);
  std::set<Lease*> seen;
  std::vector<Lease*> first;
  for (int i = 0; i < 5'000; ++i) first.push_back(table.FindOrCreate(kRule, "cust:" + std::to_string(i)));
  for (int i = 0; i < 5'000; ++i) {
    ASSERT_NE(first[i], nullptr);
    EXPECT_EQ(table.FindOrCreate(kRule, "cust:" + std::to_string(i)), first[i]);  // the same lease again
    seen.insert(first[i]);
  }
  EXPECT_EQ(seen.size(), 5'000u);
}

TEST(LeaseTable, KeysDifferingOnlyInTheirLastByteAreKeptApart) {
  LeaseTable table(1024);
  const std::string prefix(120, 'p');
  Lease* a = table.FindOrCreate(kRule, prefix + "a");
  Lease* b = table.FindOrCreate(kRule, prefix + "b");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_EQ(a->key, prefix + "a");
}

TEST(ChunkPolicy, ATinyLimitStillLeasesOneTokenAndNeverMoreThanTheLimit) {
  for (const std::uint32_t limit : {1u, 2u, 5u, 9u}) {
    const Rule r{.name = "tiny", .limit = limit, .refill_per_sec = 1};
    EXPECT_GE(ChunkCap(r), 1u) << limit;
    EXPECT_LE(ChunkMin(r), ChunkCap(r)) << limit;
    EXPECT_LE(NextChunk(ChunkCap(r), true, false, r), limit) << limit;
  }
}

TEST(ChunkPolicy, ALeasedQuotaBelowAThousandStillLeasesOne) {
  EXPECT_EQ(ChunkCap({.name = "q", .algorithm = Algorithm::kQuota, .limit = 999}), 1u);
}

TEST(ChunkPolicy, TheLargestLimitCapsTheChunkWithoutOverflow) {
  const std::uint32_t max = std::numeric_limits<std::uint32_t>::max();
  EXPECT_EQ(ChunkCap({.name = "b", .limit = max, .refill_per_sec = 1, .lease_chunk_pct = 100}), max);
  EXPECT_EQ(ChunkCap({.name = "b", .limit = max, .refill_per_sec = 1}), max / 10);
}

TEST(ChunkPolicy, DoublingAHugeChunkStaysAtTheCap) {  // chunk * 2 must not wrap around to a tiny chunk
  const Rule r{.name = "b", .limit = std::numeric_limits<std::uint32_t>::max(), .refill_per_sec = 1, .lease_chunk_pct = 100};
  EXPECT_EQ(NextChunk(1u << 31, true, false, r), ChunkCap(r));
  EXPECT_EQ(NextChunk(3'000'000'000u, true, false, r), ChunkCap(r));
}

TEST(ChunkPolicy, RepeatedHalvingStopsAtTheMinimumAndDoublingAtTheCap) {
  const Rule r{.name = "b", .limit = 1'000'000, .refill_per_sec = 1};
  std::uint32_t c = ChunkCap(r);
  for (int i = 0; i < 40; ++i) c = NextChunk(c, false, true, r);
  EXPECT_EQ(c, ChunkMin(r));
  for (int i = 0; i < 40; ++i) c = NextChunk(c, true, false, r);
  EXPECT_EQ(c, ChunkCap(r));
}

}  // namespace
}  // namespace ratelimiter
