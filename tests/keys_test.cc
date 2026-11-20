#include "redis/keys.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

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


// ---- Week 1 edge cases (2026-11-01): the slot each key lands in, as Redis Cluster computes it ----

// CRC16/XMODEM of the key's hash tag (the text between the first { and the next }, if not empty;
// else the whole key), mod 16384: Redis Cluster's KEYSLOT.
std::uint16_t Crc16(std::string_view s) {
  std::uint16_t crc = 0;
  for (const unsigned char c : s) {
    crc ^= static_cast<std::uint16_t>(c << 8);
    for (int i = 0; i < 8; ++i) {
      crc = (crc & 0x8000) != 0 ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
  }
  return crc;
}
int Slot(std::string_view key) {
  if (const auto open = key.find('{'); open != std::string_view::npos) {
    if (const auto close = key.find('}', open + 1); close != std::string_view::npos && close > open + 1) {
      key = key.substr(open + 1, close - open - 1);
    }
  }
  return Crc16(key) & 16383;
}
const Rule kBurst{.name = "burst"};
const Rule kQuota{.name = "daily_quota", .algorithm = Algorithm::kQuota};

TEST(Keys, TheSlotHelperAgreesWithRedis) {
  EXPECT_EQ(Crc16("123456789"), 0x31C3);  // CRC16/XMODEM's check value
  EXPECT_EQ(Slot("foo"), 12182);          // CLUSTER KEYSLOT foo
  EXPECT_EQ(Slot("{user1000}.following"), Slot("{user1000}.followers"));
  EXPECT_EQ(Slot("foo{}{bar}"), Crc16("foo{}{bar}") & 16383);  // an empty tag hashes the whole key
}

TEST(Keys, EveryKeyOfACustomerLandsInTheCustomersSlot) {
  for (const std::string customer : {"cust:42", "user:7", "ip:203.0.113.7", "acme|cust:42"}) {
    const int slot = Crc16(customer) & 16383;
    for (const Algorithm a : {Algorithm::kTokenBucket, Algorithm::kSlidingWindow, Algorithm::kFixedWindow, Algorithm::kQuota}) {
      EXPECT_EQ(Slot(RedisKey({.name = "r", .algorithm = a}, customer)), slot) << customer;
    }
    EXPECT_EQ(Slot(RequestKey(kQuota, customer, "req-1")), slot) << customer;
    EXPECT_EQ(Slot(PlanKey(customer)), slot) << customer;
  }
}

TEST(Keys, ATenantKeyHashesTheMerchantAndTheCustomerTogether) {  // Phase 2: one merchant's customer
  EXPECT_EQ(PlanKey("acme|cust:42"), "{acme|cust:42}:plan");
  EXPECT_NE(PlanKey("acme|cust:42"), PlanKey("other|cust:42"));  // the same customer id, another merchant
  EXPECT_EQ(Slot(RedisKey(kQuota, "acme|cust:42")), Slot(PlanKey("acme|cust:42")));
}

TEST(Keys, ARequestIdWithBracesStaysOutsideTheTag) {  // the request id comes after the customer's }
  EXPECT_EQ(Slot(RequestKey(kQuota, "cust:42", "x}{y")), Slot(RedisKey(kQuota, "cust:42")));
  EXPECT_EQ(Slot(RequestKey(kQuota, "cust:42", "{other}")), Slot(RedisKey(kQuota, "cust:42")));
}

TEST(Keys, ColonsAndNonAsciiBytesAreKeptAsTheyAre) {
  const std::string key = "user:\xc3\xb1:\xe6\x97\xa5\xe6\x9c\xac";  // user:ñ:日本 in UTF-8
  EXPECT_EQ(RedisKey(kBurst, key), "{" + key + "}:burst:tb");
  EXPECT_EQ(Slot(RedisKey(kBurst, key)), Slot(PlanKey(key)));
}

TEST(Keys, ALongestAllowedKeyIsBuiltWhole) {  // 128 bytes, the most the service accepts
  const std::string key(128, 'k');
  const std::string redis = RedisKey(kBurst, key);
  EXPECT_EQ(redis.size(), 128u + std::string("{}:burst:tb").size());
  EXPECT_EQ(redis.substr(1, 128), key);
}

TEST(Keys, CustomersSpreadEvenlyOverThreeMasters) {  // the dev cluster's slot ranges
  std::array<int, 3> per_master{};
  for (int i = 1; i <= 30'000; ++i) {
    const int slot = Slot(PlanKey("cust:" + std::to_string(i)));
    ++per_master[slot <= 5460 ? 0 : slot <= 10922 ? 1 : 2];
  }
  for (const int n : per_master) {
    EXPECT_GT(n, 9'000);  // a third is 10,000
    EXPECT_LT(n, 11'000);
  }
}

TEST(Keys, NoTwoRulesOrAlgorithmsShareAKey) {
  EXPECT_NE(RedisKey(kBurst, "c"), RedisKey({.name = "other"}, "c"));
  EXPECT_NE(RedisKey({.name = "w", .algorithm = Algorithm::kFixedWindow}, "c"),
            RedisKey({.name = "w", .algorithm = Algorithm::kSlidingWindow}, "c"));
  EXPECT_NE(RequestKey(kQuota, "c", "q"), RedisKey(kQuota, "c"));  // "...:req:q" against "...:q"
  EXPECT_NE(PlanKey("c"), RedisKey({.name = "plan"}, "c"));        // a rule named plan is "...:plan:tb"
}

}  // namespace
}  // namespace ratelimiter
