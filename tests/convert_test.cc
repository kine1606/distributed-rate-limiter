#include "server/convert.h"

#include <gtest/gtest.h>

#include <string>
#include <cstdint>
#include <limits>

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

TEST(Convert, KeyRuleAndRequestIdAreCappedAt128Bytes) {
  EXPECT_EQ(Invalid(Req(std::string(128, 'k'), "r", std::string(128, 'i'))), nullptr);
  EXPECT_NE(Invalid(Req(std::string(129, 'k'), "r")), nullptr);
  EXPECT_NE(Invalid(Req("k", "r", std::string(129, 'i'))), nullptr);
  EXPECT_EQ(Invalid(Req("k", std::string(128, 'r'))), nullptr);
  EXPECT_NE(Invalid(Req("k", std::string(129, 'r'))), nullptr);  // a rule name is looked up and hashed on every check
}

// Security review 2026-10-31: a key starting with } left Redis Cluster an empty hash tag, so one
// customer's keys spread over masters and quota.lua failed as CROSSSLOT, which a fail-open plan admits.
TEST(Convert, AKeyCannotCarryAHashTag) {
  for (const char* key : {"}x", "{x}", "a{b", "a}b", "cust:42}"}) {
    EXPECT_STREQ(Invalid(Req(key, "api_default")), "key may not contain { or }") << key;
  }
  EXPECT_EQ(Invalid(Req("cust:42", "api_default", "req-{1}")), nullptr);  // a request id sits after the tag
}

TEST(Convert, ZeroCostMeansOne) {
  auto r = Req("k", "r");
  EXPECT_EQ(ToDomain(r).cost, 1u);
  r.set_cost(7);
  EXPECT_EQ(ToDomain(r).cost, 7u);
}

TEST(Convert, RequestIdReachesTheEngine) {
  EXPECT_EQ(ToDomain(Req("k", "r", "retry-1")).request_id, "retry-1");
  EXPECT_TRUE(ToDomain(Req("k", "r")).request_id.empty());
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
  EXPECT_EQ(FillFrom({.allowed = true, .reason = Reason::kOverage}).reason(), v1::REASON_OVERAGE);
  EXPECT_EQ(FillFrom({.reason = Reason::kSuspended}).reason(), v1::REASON_SUSPENDED);
  EXPECT_EQ(ToProto(Reason::kUnknownRule), v1::REASON_UNSPECIFIED);  // sent as INVALID_ARGUMENT
}


// ---- Week 1 edge cases (2026-11-01): the API contract at its limits ----

TEST(Convert, AKeysLengthIsCountedInBytesNotCharacters) {
  std::string key;
  for (int i = 0; i < 42; ++i) key += "\xe6\x97\xa5";  // 42 three-byte characters: 126 bytes
  EXPECT_EQ(Invalid(Req(key, "r")), nullptr);
  EXPECT_NE(Invalid(Req(key + "\xe6\x97\xa5", "r")), nullptr);  // 43 of them: 129 bytes
}

TEST(Convert, TheLargestCostIsPassedOnUnchanged) {
  auto r = Req("cust:42", "api_default");
  r.set_cost(std::numeric_limits<std::uint32_t>::max());
  EXPECT_EQ(Invalid(r), nullptr);
  EXPECT_EQ(ToDomain(r).cost, std::numeric_limits<std::uint32_t>::max());
}

TEST(Convert, ARequestIdIsKeptByteForByte) {
  const std::string id = "retry 2 \xe2\x9c\x93 {x}";  // spaces, UTF-8 and braces are all allowed in a request id
  const auto r = Req("cust:42", "api_default", id);
  EXPECT_EQ(Invalid(r), nullptr);
  EXPECT_EQ(ToDomain(r).request_id, id);
  EXPECT_EQ(ToDomain(Req("cust:42", "api_default")).request_id, "");  // none: no deduplication
}

TEST(Convert, AKeyWithANulByteIsKeptWhole) {  // protobuf strings are bytes; Redis keys are too
  const std::string key("a\0b", 3);
  const auto r = Req(key, "api_default");
  EXPECT_EQ(Invalid(r), nullptr);
  EXPECT_EQ(ToDomain(r).key.size(), 3u);
}

}  // namespace
}  // namespace ratelimiter
