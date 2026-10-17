#include "server/convert.h"

#include <gtest/gtest.h>

#include <string>

namespace ratelimiter {
namespace {

v1::CheckRequest Req(std::string key, std::string rule, std::string request_id = "") {
  v1::CheckRequest r;
  r.set_key(std::move(key));
  r.set_rule(std::move(rule));
  r.set_request_id(std::move(request_id));
  return r;
}

v1::CheckResponse FillFrom(const CheckResult& r) {
  v1::CheckResponse out;
  Fill(r, &out);
  return out;
}

TEST(Convert, ValidRequestPasses) { EXPECT_EQ(Invalid(Req("cust:42", "api_default")), nullptr); }

TEST(Convert, EmptyKeyOrRuleIsInvalid) {
  EXPECT_NE(Invalid(Req("", "api_default")), nullptr);
  EXPECT_NE(Invalid(Req("cust:42", "")), nullptr);
}

TEST(Convert, KeyAndRequestIdAreCappedAt128Bytes) {
  EXPECT_EQ(Invalid(Req(std::string(128, 'k'), "r", std::string(128, 'i'))), nullptr);
  EXPECT_NE(Invalid(Req(std::string(129, 'k'), "r")), nullptr);
  EXPECT_NE(Invalid(Req("k", "r", std::string(129, 'i'))), nullptr);
}

TEST(Convert, ZeroCostMeansOne) {
  auto r = Req("k", "r");
  EXPECT_EQ(ToDomain(r).cost, 1u);
  r.set_cost(7);
  EXPECT_EQ(ToDomain(r).cost, 7u);
}

TEST(Convert, FillCopiesEveryField) {
  const auto out = FillFrom({.allowed = true, .reason = Reason::kAllowed, .remaining = 3,
                             .retry_after_ms = 4, .limit = 5, .reset_after_ms = 6});
  EXPECT_TRUE(out.allowed());
  EXPECT_EQ(out.reason(), v1::REASON_OK);
  EXPECT_EQ(out.remaining(), 3u);
  EXPECT_EQ(out.retry_after_ms(), 4u);
  EXPECT_EQ(out.limit(), 5u);
  EXPECT_EQ(out.reset_after_ms(), 6u);
}

// A wrong mapping turns "Redis is down" into "slow down" at the gateway (503 vs 429).
TEST(Convert, ReasonMapping) {
  EXPECT_EQ(FillFrom({.reason = Reason::kRateLimited}).reason(), v1::REASON_RATE_LIMITED);
  EXPECT_EQ(FillFrom({.reason = Reason::kQuotaExhausted}).reason(), v1::REASON_QUOTA_EXHAUSTED);
  EXPECT_EQ(FillFrom({.reason = Reason::kUnavailable}).reason(), v1::REASON_UNAVAILABLE);
  EXPECT_EQ(FillFrom({.allowed = true, .reason = Reason::kUnverified}).reason(), v1::REASON_UNVERIFIED);
  EXPECT_EQ(ToProto(Reason::kUnknownRule), v1::REASON_UNSPECIFIED);  // sent as INVALID_ARGUMENT
}

}  // namespace
}  // namespace ratelimiter
