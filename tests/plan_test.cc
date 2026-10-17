#include "plan/plan.h"

#include <gtest/gtest.h>

namespace ratelimiter {
namespace {

TEST(Plan, ParsesTheSeededHash) {
  const Plan p = ParsePlan({{"plan", "pay"}, {"daily", "2000"}, {"burst", "20"}, {"refill", "20"},
                            {"overage", "grace"}, {"extra", "200"}, {"failure", "open"}});
  EXPECT_EQ(p.daily, 2000u);
  EXPECT_EQ(p.burst, 20u);
  EXPECT_EQ(p.refill, 20u);
}

TEST(Plan, MissingHashIsFree) {
  const Plan p = ParsePlan({});
  EXPECT_EQ(p.daily, 200u);
  EXPECT_EQ(p.burst, 2u);
  EXPECT_EQ(p.refill, 2u);
}

TEST(Plan, MalformedFieldsKeepTheFreeDefault) {
  const Plan p = ParsePlan({{"daily", "abc"}, {"burst", "-5"}, {"refill", "10x"}});
  EXPECT_EQ(p.daily, 200u);
  EXPECT_EQ(p.burst, 2u);
  EXPECT_EQ(p.refill, 2u);
  EXPECT_EQ(ParsePlan({{"daily", ""}}).daily, 200u);
  EXPECT_EQ(ParsePlan({{"daily", "99999999999"}}).daily, 200u);  // overflows uint32
}

TEST(Plan, ZeroIsAValidLimit) {  // the suspended preset
  const Plan p = ParsePlan({{"daily", "0"}, {"burst", "0"}, {"refill", "0"}});
  EXPECT_EQ(p.daily, 0u);
  EXPECT_EQ(p.burst, 0u);
  EXPECT_EQ(p.refill, 0u);
}

}  // namespace
}  // namespace ratelimiter
