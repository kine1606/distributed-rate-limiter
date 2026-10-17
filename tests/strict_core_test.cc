#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "core/strict_core.h"

namespace ratelimiter {
namespace {

// Answers every Eval with a canned result and records what it was asked.
class FakeBackend final : public RedisBackend {
 public:
  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, ScriptCallback done) override {
    calls.push_back({rule.name, std::string(key), requested});
    done(reply);
  }

  struct Call {
    std::string rule;
    std::string key;
    std::uint32_t requested;
  };
  std::vector<Call> calls;
  ScriptResult reply;
};

RuleTable Rules() {
  Rule open{.name = "open", .limit = 10, .refill_per_sec = 1, .failure_policy = FailurePolicy::kFailOpen};
  Rule closed{.name = "closed", .limit = 10, .refill_per_sec = 1, .failure_policy = FailurePolicy::kFailClosed};
  return {{"open", open}, {"closed", closed}};
}

CheckResult RunCheck(StrictCore& core, std::string_view key, std::string_view rule, std::uint32_t cost = 1) {
  CheckResult out;
  bool called = false;
  core.Check({key, rule, cost}, [&](const CheckResult& r) {
    out = r;
    called = true;
  });
  EXPECT_TRUE(called);
  return out;
}

TEST(StrictCore, AllowedReplyPassesThroughWithLimit) {
  FakeBackend redis;
  redis.reply = {.allowed = true, .remaining = 7};
  StrictCore core(redis, Rules());

  const auto r = RunCheck(core, "user:1", "open", 3);
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kAllowed);
  EXPECT_EQ(r.remaining, 7u);
  EXPECT_EQ(r.limit, 10u);
  ASSERT_EQ(redis.calls.size(), 1u);
  EXPECT_EQ(redis.calls[0].key, "user:1");
  EXPECT_EQ(redis.calls[0].rule, "open");
  EXPECT_EQ(redis.calls[0].requested, 3u);
}

TEST(StrictCore, EmptyBucketIsRateLimitedWithRetryAfter) {
  FakeBackend redis;
  redis.reply = {.allowed = false, .remaining = 0, .retry_after_ms = 400};
  StrictCore core(redis, Rules());

  const auto r = RunCheck(core, "user:1", "open");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kRateLimited);
  EXPECT_EQ(r.retry_after_ms, 400u);
}

TEST(StrictCore, UnknownRuleNeverReachesRedis) {
  FakeBackend redis;
  StrictCore core(redis, Rules());

  const auto r = RunCheck(core, "user:1", "no_such_rule");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnknownRule);
  EXPECT_TRUE(redis.calls.empty());
}

TEST(StrictCore, RedisDownFailOpenAllows) {
  FakeBackend redis;
  redis.reply = {.redis_error = true};
  StrictCore core(redis, Rules());

  const auto r = RunCheck(core, "user:1", "open");
  EXPECT_TRUE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnverified);
}

TEST(StrictCore, RedisDownFailClosedIsUnavailable) {
  FakeBackend redis;
  redis.reply = {.redis_error = true};
  StrictCore core(redis, Rules());

  const auto r = RunCheck(core, "user:1", "closed");
  EXPECT_FALSE(r.allowed);
  EXPECT_EQ(r.reason, Reason::kUnavailable);
}

}  // namespace
}  // namespace ratelimiter
