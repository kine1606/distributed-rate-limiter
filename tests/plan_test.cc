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
  EXPECT_EQ(p.overage, Overage::kGrace);
  EXPECT_EQ(p.extra, 200u);
  EXPECT_FALSE(p.suspended);
}

TEST(Plan, OveragePolicyAndSuspension) {
  EXPECT_EQ(ParsePlan({}).overage, Overage::kHard);  // Free
  EXPECT_EQ(ParsePlan({{"overage", "cap"}}).overage, Overage::kCap);
  EXPECT_EQ(ParsePlan({{"overage", "unlimited"}}).overage, Overage::kHard);  // unknown: the strictest
  EXPECT_TRUE(ParsePlan({{"plan", "suspended"}}).suspended);
  EXPECT_FALSE(ParsePlan({{"plan", "free"}}).suspended);
}

TEST(Plan, QuotaStampChangesWithTheQuotaTerms) {
  const Plan free = ParsePlan({});
  EXPECT_EQ(QuotaStamp(free), QuotaStamp(ParsePlan({{"burst", "20"}})));  // burst is not a quota term
  EXPECT_NE(QuotaStamp(free), QuotaStamp(ParsePlan({{"daily", "2000"}})));
  EXPECT_NE(QuotaStamp(free), QuotaStamp(ParsePlan({{"extra", "1"}})));
  EXPECT_NE(QuotaStamp(ParsePlan({{"overage", "grace"}, {"extra", "5"}})),
            QuotaStamp(ParsePlan({{"overage", "cap"}, {"extra", "5"}})));
}

TEST(Plan, FailurePolicyIsOpenOnlyWhenTheHashSaysSo) {  // Week 5: paid open, Free closed
  EXPECT_EQ(ParsePlan({}).failure, FailurePolicy::kFailClosed);
  EXPECT_EQ(ParsePlan({{"failure", "open"}}).failure, FailurePolicy::kFailOpen);
  EXPECT_EQ(ParsePlan({{"failure", "closed"}}).failure, FailurePolicy::kFailClosed);
  EXPECT_EQ(ParsePlan({{"failure", "OPEN"}}).failure, FailurePolicy::kFailClosed);  // unknown: the safe side
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

TEST(Plan, PeriodAndBoost) {  // Phase 2, P1
  const Plan p = ParsePlan({{"daily", "50000"}, {"period", "month"}, {"boost", "500"}, {"boost_until", "1792900000000"}});
  EXPECT_EQ(p.period, QuotaPeriod::kMonth);
  EXPECT_EQ(p.boost, 500u);
  EXPECT_EQ(p.boost_until_ms, 1792900000000);
  const Plan bad = ParsePlan({{"period", "week"}, {"boost", "-3"}, {"boost_until", "soon"}});
  EXPECT_EQ(bad.period, QuotaPeriod::kDay);  // unknown: the daily default
  EXPECT_EQ(bad.boost, 0u);
  EXPECT_EQ(bad.boost_until_ms, 0);
}

TEST(Plan, QuotaStampChangesWithABoostOrThePeriod) {
  const Plan base = ParsePlan({{"daily", "2000"}});
  Plan boosted = base, longer = base, monthly = base;
  boosted.boost = 100;
  longer.boost = 100;
  longer.boost_until_ms = 5;
  monthly.period = QuotaPeriod::kMonth;
  EXPECT_NE(QuotaStamp(base), QuotaStamp(boosted));
  EXPECT_NE(QuotaStamp(boosted), QuotaStamp(longer));
  EXPECT_NE(QuotaStamp(base), QuotaStamp(monthly));
  EXPECT_EQ(QuotaStamp(base), QuotaStamp(ParsePlan({{"daily", "2000"}})));
}

}  // namespace
}  // namespace ratelimiter
