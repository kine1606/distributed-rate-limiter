// Lua script tests against a REAL Redis (no mocks: the whole point is Redis semantics).
// Redis address: env RL_TEST_REDIS="host:port" (default 127.0.0.1:6380).
// If Redis is unreachable the tests are skipped, not failed.
#include <hiredis/hiredis.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct Reply {
  std::vector<long long> ints;
  bool error = false;
  std::string err;
};

class LuaTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string host = "127.0.0.1";
    int port = 6380;
    if (const char* e = std::getenv("RL_TEST_REDIS")) {
      std::string a = e;
      auto c = a.find(':');
      host = a.substr(0, c);
      if (c != std::string::npos) port = std::stoi(a.substr(c + 1));
    }
    host_ = host;
    port_ = port;
    ctx_.reset(redisConnect(host.c_str(), port));
    if (!ctx_ || ctx_->err) GTEST_SKIP() << "Redis not reachable at " << host << ":" << port;
    tb_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/token_bucket.lua");
    quota_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/quota.lua");
    fw_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/fixed_window.lua");
    sw_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/sliding_window.lua");
    return_sha_ = Load(std::string(RL_SOURCE_DIR) + "/lua/return_lease.lua");
  }

  std::string Load(const std::string& path) {
    std::string src = ReadFile(path);
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "SCRIPT LOAD %b", src.data(), src.size()));
    EXPECT_TRUE(r && r->type == REDIS_REPLY_STRING) << path;
    std::string sha = r ? r->str : "";
    freeReplyObject(r);
    return sha;
  }

  Reply Run(const std::string& sha, const std::string& key, const std::vector<std::string>& args) {
    return RunKeys(sha, {key}, args);
  }
  Reply RunKeys(const std::string& sha, const std::vector<std::string>& keys, const std::vector<std::string>& args) {
    const std::string nkeys = std::to_string(keys.size());
    std::vector<const char*> argv{"EVALSHA", sha.c_str(), nkeys.c_str()};
    for (auto& k : keys) argv.push_back(k.c_str());
    for (auto& a : args) argv.push_back(a.c_str());
    auto* r = static_cast<redisReply*>(redisCommandArgv(ctx_.get(), argv.size(), argv.data(), nullptr));
    Reply out;
    if (!r || r->type == REDIS_REPLY_ERROR) {
      out.error = true;
      out.err = r ? r->str : "null";
    } else {
      for (size_t i = 0; i < r->elements; ++i) out.ints.push_back(r->element[i]->integer);
    }
    if (r) freeReplyObject(r);
    return out;
  }

  long long Pttl(const std::string& key) {
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "PTTL %s", key.c_str()));
    long long v = r->integer;
    freeReplyObject(r);
    return v;
  }

  void Del(const std::string& key) { freeReplyObject(redisCommand(ctx_.get(), "DEL %s", key.c_str())); }

  // Token bucket: capacity, refill/s, requested, ttl_ms
  Reply TB(const std::string& key, int cap, int rate, int req, int ttl = 60000) {
    return Run(tb_sha_, key, {std::to_string(cap), std::to_string(rate), std::to_string(req), std::to_string(ttl)});
  }
  Reply Quota(const std::string& key, int limit, int cost) {
    return Run(quota_sha_, key, {std::to_string(limit), std::to_string(cost)});
  }
  // quota.lua v2: overage policy and extra; `req_key` non-empty deduplicates by request id.
  Reply QuotaV2(const std::string& key, int limit, int cost, const std::string& policy, int extra,
                const std::string& req_key = "") {
    std::vector<std::string> keys{key};
    if (!req_key.empty()) keys.push_back(req_key);
    return RunKeys(quota_sha_, keys, {std::to_string(limit), std::to_string(cost), policy, std::to_string(extra)});
  }
  // quota.lua with a period ("day" | "month") and a boost of `boost` units until `boost_until_ms`.
  Reply QuotaV3(const std::string& key, int limit, const std::string& period, int boost = 0,
                long long boost_until_ms = 0) {
    return Run(quota_sha_, key, {std::to_string(limit), "1", "hard", "0", period, std::to_string(boost),
                                 std::to_string(boost_until_ms)});
  }
  // quota.lua with a prepaid top-up of `topup` units bought for `topup_period` ("d<day>" or "m<month>").
  Reply QuotaTopUp(const std::string& key, int limit, int topup, const std::string& topup_period) {
    return Run(quota_sha_, key, {std::to_string(limit), "1", "hard", "0", "day", "0", "0", std::to_string(topup),
                                 topup_period});
  }
  // The month starting on day `anchor` (P5; 1: the UTC calendar month) that `ms` falls in, numbered as
  // the script numbers it (year * 12 + month - 1 of the month it starts in), and the ms to the next one.
  static std::pair<long long, long long> MonthOf(long long ms, unsigned anchor = 1) {
    using namespace std::chrono;
    const year_month_day ymd{floor<days>(sys_time<milliseconds>(milliseconds(ms)))};
    year_month start = ymd.year() / ymd.month();
    if (static_cast<unsigned>(ymd.day()) < anchor) start -= months(1);
    const sys_days next{(start + months(1)) / day(anchor)};
    return {static_cast<int>(start.year()) * 12LL + static_cast<unsigned>(start.month()) - 1,
            duration_cast<milliseconds>(next.time_since_epoch()).count() - ms};
  }
  // Today's quota state, as if `used` units were already consumed (request used + 1 comes next).
  void SetUsedToday(const std::string& key, int used) {
    const std::string day = std::to_string(RedisNowMs() / 86400000);
    freeReplyObject(redisCommand(ctx_.get(), "HSET %s day %s used %d", key.c_str(), day.c_str(), used));
  }
  std::string HGet(const std::string& key, const std::string& field) {
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "HGET %s %s", key.c_str(), field.c_str()));
    std::string v = r && r->type == REDIS_REPLY_STRING ? r->str : "";
    freeReplyObject(r);
    return v;
  }
  // Fixed / sliding window: limit, window_ms, requested
  Reply FW(const std::string& key, int limit, int window_ms, int req = 1) {
    return Run(fw_sha_, key, {std::to_string(limit), std::to_string(window_ms), std::to_string(req)});
  }
  Reply SW(const std::string& key, int limit, int window_ms, int req = 1) {
    return Run(sw_sha_, key, {std::to_string(limit), std::to_string(window_ms), std::to_string(req)});
  }

  long long RedisNowMs() {
    auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "TIME"));
    long long ms = std::stoll(r->element[0]->str) * 1000 + std::stoll(r->element[1]->str) / 1000;
    freeReplyObject(r);
    return ms;
  }
  // Sleeps until `offset_ms` past the next multiple of `window_ms` on Redis's clock.
  void SleepPastNextBoundary(int window_ms, int offset_ms) {
    const long long now = RedisNowMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(window_ms - now % window_ms + offset_ms));
  }
  // Timing tests sleep on the monotonic clock but the scripts read Redis TIME (wall clock).
  // WSL2 steps its wall clock by hundreds of ms when it resyncs with the host, so those tests
  // check this before asserting and skip if the two clocks drifted apart.
  void StartClockCheck() {
    mono_start_ = std::chrono::steady_clock::now();
    wall_start_ = RedisNowMs();
  }
  long long ClockStepMs() {
    const auto mono = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - mono_start_).count();
    return RedisNowMs() - wall_start_ - mono;
  }
  // Sliding window, limit 5, window 1 s: denied now, still denied 50 ms before retry_after,
  // allowed 20 ms after it. Call StartClockCheck() before setting up the state.
  void ExpectRetryAfter(const std::string& k) {
    auto d = SW(k, 5, 1000);
    const long long retry = d.ints[2];
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(retry - 50, 0LL)));
    auto early = SW(k, 5, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    auto late = SW(k, 5, 1000);
    if (std::abs(ClockStepMs()) > 10) GTEST_SKIP() << "wall clock stepped " << ClockStepMs() << " ms";
    ASSERT_EQ(d.ints[0], 0);
    ASSERT_GT(retry, 60);
    EXPECT_EQ(early.ints[0], 0) << "allowed 50 ms before retry_after=" << retry;
    EXPECT_EQ(late.ints[0], 1) << "still denied 20 ms after retry_after=" << retry;
  }

  struct Closer { void operator()(redisContext* c) const { redisFree(c); } };
  std::unique_ptr<redisContext, Closer> ctx_;
  std::string host_;
  int port_ = 0;
  std::string tb_sha_, quota_sha_, fw_sha_, sw_sha_, return_sha_;
  std::chrono::steady_clock::time_point mono_start_;
  long long wall_start_ = 0;
};

// ---- token bucket ----

TEST_F(LuaTest, BucketAdmitsBurstThenDenies) {
  const std::string k = "{t1}:tb";
  Del(k);
  for (int i = 4; i >= 0; --i) {
    auto r = TB(k, 5, 1, 1);
    ASSERT_FALSE(r.error) << r.err;
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
  }
  auto d = TB(k, 5, 1, 1);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_GT(d.ints[2], 0);       // told when to retry
  EXPECT_LE(d.ints[2], 1000);    // at 1 token/s, at most one second
}

TEST_F(LuaTest, CostLargerThanBurstIsNeverAllowed) {
  const std::string k = "{t2}:tb";
  Del(k);
  auto r = TB(k, 5, 1, 10);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[2], 0);       // waiting would not help
  EXPECT_EQ(r.ints[1], 5);       // bucket untouched
}

TEST_F(LuaTest, RequestedChunkTakesManyTokensAtOnce) {
  const std::string k = "{t3}:tb";
  Del(k);
  auto r = TB(k, 100, 10, 40);   // a lease refill of 40
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 60);
}

TEST_F(LuaTest, RefillsOverTime) {
  const std::string k = "{t4}:tb";
  Del(k);
  TB(k, 10, 10, 10);             // drain
  EXPECT_EQ(TB(k, 10, 10, 1).ints[0], 0);
  std::this_thread::sleep_for(std::chrono::milliseconds(350));  // ~3 tokens
  auto r = TB(k, 10, 10, 2);
  EXPECT_EQ(r.ints[0], 1);
}

TEST_F(LuaTest, ClockJumpBackwardsNeverCreatesTokens) {
  const std::string k = "{t5}:tb";
  Del(k);
  TB(k, 5, 1, 5);                // drain
  // Simulate the stored timestamp being far in the future (clock moved back).
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s last_refill_ms 99999999999999", k.c_str()));
  auto r = TB(k, 5, 1, 1);
  EXPECT_EQ(r.ints[0], 0);       // elapsed clamped to 0, no free tokens
}

TEST_F(LuaTest, IdleKeysCarryTtlAndExpire) {
  const std::string k = "{t6}:tb";
  Del(k);
  TB(k, 5, 1, 1, 200);
  long long ttl = Pttl(k);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 200);
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  EXPECT_EQ(Pttl(k), -2);        // key is gone
  EXPECT_EQ(TB(k, 5, 1, 1).ints[1], 4);  // and a new bucket starts full
}

TEST_F(LuaTest, ConcurrentCallersNeverOverAdmit) {
  const std::string k = "{t7}:tb";
  Del(k);
  std::atomic<int> admitted{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 8; ++t) {
    ts.emplace_back([&] {
      redisContext* c = redisConnect("127.0.0.1", 6380);
      if (const char* e = std::getenv("RL_TEST_REDIS")) {
        redisFree(c);
        std::string a = e;
        auto p = a.find(':');
        c = redisConnect(a.substr(0, p).c_str(), std::stoi(a.substr(p + 1)));
      }
      for (int i = 0; i < 50; ++i) {
        auto* r = static_cast<redisReply*>(redisCommand(c, "EVALSHA %s 1 %s 100 0 1 60000", tb_sha_.c_str(), k.c_str()));
        if (r && r->type == REDIS_REPLY_ARRAY && r->element[0]->integer == 1) ++admitted;
        if (r) freeReplyObject(r);
      }
      redisFree(c);
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(admitted.load(), 100);   // 400 attempts, rate 0, capacity 100 => exactly 100
}

// ---- daily quota ----

TEST_F(LuaTest, QuotaCountsUpToLimit) {
  const std::string k = "{q1}:quota";
  Del(k);
  for (int i = 1; i <= 3; ++i) {
    auto r = Quota(k, 3, 1);
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
    EXPECT_EQ(r.ints[2], 3 - i);
  }
  auto d = Quota(k, 3, 1);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_EQ(d.ints[1], 3);       // a denied request is not counted
  EXPECT_GT(d.ints[3], 0);
  EXPECT_LE(d.ints[3], 86400000);
}

TEST_F(LuaTest, QuotaUpgradeMidDayKeepsUsageAndAppliesImmediately) {
  const std::string k = "{q2}:quota";
  Del(k);
  for (int i = 0; i < 3; ++i) Quota(k, 3, 1);
  EXPECT_EQ(Quota(k, 3, 1).ints[0], 0);
  auto r = Quota(k, 10, 1);      // customer upgraded
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 4);
  EXPECT_EQ(r.ints[2], 6);
}

TEST_F(LuaTest, QuotaDowngradeBelowUsageDeniesWithoutNegativeRemaining) {
  const std::string k = "{q3}:quota";
  Del(k);
  for (int i = 0; i < 5; ++i) Quota(k, 10, 1);
  auto r = Quota(k, 2, 1);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[2], 0);
}

TEST_F(LuaTest, QuotaCostGreaterThanRemainingIsDeniedAndNotCounted) {
  const std::string k = "{q4}:quota";
  Del(k);
  Quota(k, 5, 3);
  auto r = Quota(k, 5, 3);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[1], 3);
}

TEST_F(LuaTest, QuotaResetsOnNewUtcDay) {
  const std::string k = "{q5}:quota";
  Del(k);
  for (int i = 0; i < 3; ++i) Quota(k, 3, 1);
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s day 1", k.c_str()));  // pretend it is yesterday's data
  auto r = Quota(k, 3, 1);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 1);
}

TEST_F(LuaTest, QuotaKeyExpiresAfterDayEnds) {
  const std::string k = "{q6}:quota";
  Del(k);
  Quota(k, 3, 1);
  long long ttl = Pttl(k);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 86400000 + 3600000);
}

// ---- quota v3: monthly periods and boosts (Phase 2, P1) ----

TEST_F(LuaTest, MonthlyQuotaCountsAcrossDaysAndStartsAgainInANewMonth) {
  const std::string k = "{qm1}:quota";
  Del(k);
  const auto [month, until_next] = MonthOf(RedisNowMs());
  // 4 used earlier this month, on other days: a monthly quota of 5 has one left.
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s month %lld used 4", k.c_str(), month));
  const auto r = QuotaV3(k, 5, "month");
  ASSERT_EQ(r.ints.size(), 7u);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[2], 0);
  EXPECT_NEAR(static_cast<double>(r.ints[3]), static_cast<double>(until_next), 2000.0);  // to the 1st, 00:00 UTC
  EXPECT_EQ(QuotaV3(k, 5, "month").ints[0], 0);
  EXPECT_EQ(HGet(k, "month"), std::to_string(month));
  EXPECT_EQ(HGet(k, "day"), "");
  EXPECT_GT(Pttl(k), until_next - 2000);  // kept past the month's end, for the invoice

  // Last month full: this month starts at zero.
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s month %lld used 5", k.c_str(), month - 1));
  const auto fresh = QuotaV3(k, 5, "month");
  EXPECT_EQ(fresh.ints[0], 1);
  EXPECT_EQ(fresh.ints[1], 1);
}

// P5: billing from the subscription date. Redis's clock cannot be moved, so the anchors are around
// today: on or before today the period began this month; after today (only possible up to the 27th)
// it began last month. The property tests cover every date on a fake clock.
TEST_F(LuaTest, AnAnchoredMonthRunsFromItsDayToTheSameDayOfTheNextMonth) {
  const std::string k = "{qa1}:quota";
  const long long now = RedisNowMs();
  const unsigned today = static_cast<unsigned>(std::chrono::year_month_day{
      std::chrono::floor<std::chrono::days>(std::chrono::sys_time<std::chrono::milliseconds>(std::chrono::milliseconds(now)))}
                                                   .day());
  std::vector<unsigned> anchors{std::min(today, 28u)};
  if (today < 28) anchors.push_back(today + 1);
  for (const unsigned anchor : anchors) {
    Del(k);
    const auto [month, until_next] = MonthOf(now, anchor);
    const auto r = Run(quota_sha_, k, {"5", "1", "hard", "0", "month", "0", "0", "0", "", std::to_string(anchor)});
    ASSERT_EQ(r.ints.size(), 7u);
    EXPECT_EQ(r.ints[0], 1) << anchor;
    EXPECT_NEAR(static_cast<double>(r.ints[3]), static_cast<double>(until_next), 2000.0) << anchor;
    EXPECT_EQ(HGet(k, "month"), std::to_string(month)) << anchor;
    EXPECT_EQ(HGet(k, "bm"), std::to_string(month)) << anchor;  // the billing month follows the anchor too
  }
  Del(k);  // a day out of range is the calendar month
  Run(quota_sha_, k, {"5", "1", "hard", "0", "month", "0", "0", "0", "", "31"});
  EXPECT_EQ(HGet(k, "month"), std::to_string(MonthOf(now).first));
}

TEST_F(LuaTest, ABoostRaisesTheLimitUntilItEnds) {
  const std::string k = "{qb1}:quota", ended = "{qb2}:quota";
  Del(k);
  Del(ended);
  const long long now = RedisNowMs();
  for (int i = 1; i <= 3; ++i) {  // a limit of 1, plus 2 for the next minute
    const auto r = QuotaV3(k, 1, "day", 2, now + 60000);
    EXPECT_EQ(r.ints[0], 1) << "call " << i;
    EXPECT_EQ(r.ints[6], 3);
  }
  EXPECT_EQ(QuotaV3(k, 1, "day", 2, now + 60000).ints[0], 0);
  const auto r = QuotaV3(ended, 1, "day", 2, now - 1000);  // the boost ended a second ago
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[6], 1);
  EXPECT_EQ(QuotaV3(ended, 1, "day", 2, now - 1000).ints[0], 0);
}

// ---- quota v4: prepaid top-ups and alerts (Phase 2, P2) ----

TEST_F(LuaTest, ATopUpCountsOnlyInThePeriodItWasBoughtFor) {
  const std::string k = "{qt1}:quota", old = "{qt2}:quota";
  Del(k);
  Del(old);
  const std::string today = "d" + std::to_string(RedisNowMs() / 86400000);
  for (int i = 1; i <= 3; ++i) EXPECT_EQ(QuotaTopUp(k, 1, 2, today).ints[0], 1) << "call " << i;
  const auto denied = QuotaTopUp(k, 1, 2, today);
  EXPECT_EQ(denied.ints[0], 0);
  EXPECT_EQ(denied.ints[6], 3);  // 1 + the top-up
  const std::string yesterday = "d" + std::to_string(RedisNowMs() / 86400000 - 1);
  const auto r = QuotaTopUp(old, 1, 2, yesterday);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[6], 1);  // bought for another period: no effect
  EXPECT_EQ(QuotaTopUp(old, 1, 2, yesterday).ints[0], 0);
}

TEST_F(LuaTest, EachAlertIsPublishedOncePerPeriodAndNeverForAReplay) {
  std::unique_ptr<redisContext, Closer> sub(redisConnect(host_.c_str(), port_));
  ASSERT_TRUE(sub && !sub->err);
  freeReplyObject(redisCommand(sub.get(), "SUBSCRIBE rl-alerts"));
  redisSetTimeout(sub.get(), timeval{2, 0});
  const auto next = [&sub]() -> std::string {  // the next message on rl-alerts
    void* reply = nullptr;
    if (redisGetReply(sub.get(), &reply) != REDIS_OK || reply == nullptr) return "(none)";
    auto* r = static_cast<redisReply*>(reply);
    std::string payload = r->type == REDIS_REPLY_ARRAY && r->elements == 3 ? r->element[2]->str : "(other)";
    freeReplyObject(r);
    return payload;
  };
  const std::string k = "{acme|ca1}:daily_quota:q", req = "{acme|ca1}:daily_quota:req:x";
  Del(k);
  Del(req);
  std::vector<int> allowed;
  for (int i = 1; i <= 7; ++i) allowed.push_back(static_cast<int>(QuotaV2(k, 5, 1, "cap", 2).ints[0]));
  EXPECT_EQ(allowed, (std::vector<int>{1, 1, 1, 1, 1, 1, 1}));  // 5 in the plan, 2 billed
  EXPECT_EQ(QuotaV2(k, 5, 1, "cap", 2, req).ints[0], 0);      // the first denial
  EXPECT_EQ(QuotaV2(k, 5, 1, "cap", 2, req).ints[5], 1);      // its replay
  EXPECT_EQ(QuotaV2(k, 5, 1, "cap", 2).ints[0], 0);           // a second denial
  freeReplyObject(redisCommand(ctx_.get(), "PUBLISH rl-alerts end"));

  const std::vector<std::pair<std::string, std::string>> want{
      {"quota.80", "\"used\":4"}, {"quota.100", "\"used\":5"}, {"overage.started", "\"overage_used\":1"},
      {"quota.denied", "\"overage_used\":2"}};
  for (const auto& [event, detail] : want) {
    const std::string m = next();
    EXPECT_NE(m.find("\"event\":\"" + event + "\""), std::string::npos) << m;
    EXPECT_NE(m.find(detail), std::string::npos) << m;
    EXPECT_NE(m.find("\"customer\":\"acme|ca1\""), std::string::npos) << m;
    EXPECT_NE(m.find("\"limit\":5"), std::string::npos) << m;
  }
  EXPECT_EQ(next(), "end");  // nothing more: not for the replay, not for the second denial

  // A new period starts the alerts again.
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s day %lld", k.c_str(), RedisNowMs() / 86400000 - 1));
  for (int i = 1; i <= 4; ++i) QuotaV2(k, 5, 1, "cap", 2);
  freeReplyObject(redisCommand(ctx_.get(), "PUBLISH rl-alerts end"));
  const std::string again = next();
  EXPECT_NE(again.find("\"event\":\"quota.80\""), std::string::npos) << again;
  EXPECT_EQ(next(), "end");
}

// ---- quota v2: overage policies and request id (Week 4) ----

// The Week 4 gate on the script: the same request 2,001 on three plans gives three answers.
TEST_F(LuaTest, Request2001OnHardGraceAndCapGivesThreeDifferentAnswers) {
  const std::string hard = "{q7h}:quota", grace = "{q7g}:quota", cap = "{q7c}:quota";
  for (const auto& k : {hard, grace, cap}) {
    Del(k);
    SetUsedToday(k, 2000);
  }
  const auto h = QuotaV2(hard, 2000, 1, "hard", 0);
  const auto g = QuotaV2(grace, 2000, 1, "grace", 200);
  const auto c = QuotaV2(cap, 2000, 1, "cap", 1000);
  ASSERT_EQ(h.ints.size(), 7u);
  EXPECT_EQ(h.ints[6], 2000);  // the limit in force
  EXPECT_EQ(h.ints[0], 0);  // hard: denied
  EXPECT_EQ(h.ints[4], 0);
  EXPECT_EQ(g.ints[0], 1);  // grace: allowed, free
  EXPECT_EQ(g.ints[4], 0);
  EXPECT_EQ(g.ints[2], 0);  // nothing left of the plan itself
  EXPECT_EQ(c.ints[0], 1);  // cap: allowed, billed as overage
  EXPECT_EQ(c.ints[4], 1);
  EXPECT_EQ(HGet(cap, "used"), "2000");  // overage is counted apart from the plan
  EXPECT_EQ(HGet(cap, "overage_used"), "1");
}

TEST_F(LuaTest, GraceAdmitsExactlyTheExtraThenDenies) {
  const std::string k = "{q8}:quota";
  Del(k);
  for (int i = 1; i <= 5; ++i) EXPECT_EQ(QuotaV2(k, 3, 1, "grace", 2).ints[0], 1) << "request " << i;
  const auto d = QuotaV2(k, 3, 1, "grace", 2);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_EQ(d.ints[1], 5);  // the denial is not counted
}

TEST_F(LuaTest, CapAdmitsExactlyTheCapAsOverageThenDenies) {
  const std::string k = "{q9}:quota";
  Del(k);
  for (int i = 0; i < 3; ++i) EXPECT_EQ(QuotaV2(k, 3, 1, "cap", 2).ints[4], 0);  // within the plan
  for (int i = 0; i < 2; ++i) {
    const auto r = QuotaV2(k, 3, 1, "cap", 2);
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[4], 1);
  }
  EXPECT_EQ(QuotaV2(k, 3, 1, "cap", 2).ints[0], 0);
  EXPECT_EQ(HGet(k, "used"), "3");
  EXPECT_EQ(HGet(k, "overage_used"), "2");
}

TEST_F(LuaTest, CapBillsOnlyThePartOfARequestBeyondTheLimit) {
  const std::string k = "{q10}:quota";
  Del(k);
  SetUsedToday(k, 2);
  const auto r = QuotaV2(k, 3, 2, "cap", 5);  // one unit still in the plan, one beyond it
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[4], 1);
  EXPECT_EQ(HGet(k, "used"), "3");
  EXPECT_EQ(HGet(k, "overage_used"), "1");
}

// ---- billing totals per calendar month (Phase 2, P3) ----

TEST_F(LuaTest, TheMonthCountsUnitsInThePlanAndBilledOverageSeparately) {
  const std::string cap = "{qb1}:quota", grace = "{qb2}:quota";
  Del(cap);
  Del(grace);
  const long long month = MonthOf(RedisNowMs()).first;
  for (int i = 0; i < 2; ++i) QuotaV2(cap, 3, 1, "cap", 5);
  QuotaV2(cap, 3, 2, "cap", 5);                     // straddles the limit: 1 in the plan, 1 billed
  QuotaV2(cap, 3, 3, "cap", 5);                     // all billed
  EXPECT_EQ(QuotaV2(cap, 3, 2, "cap", 5).ints[0], 0);  // past the cap: denied, counts nowhere
  EXPECT_EQ(HGet(cap, "bm"), std::to_string(month));
  EXPECT_EQ(HGet(cap, "bu"), "3");
  EXPECT_EQ(HGet(cap, "bo"), "4");
  for (int i = 0; i < 5; ++i) QuotaV2(grace, 2, 1, "grace", 2);  // 2 in the plan, 2 free, 1 denied
  EXPECT_EQ(HGet(grace, "bu"), "4");
  EXPECT_EQ(HGet(grace, "bo"), "0");
}

TEST_F(LuaTest, ARetriedRequestIsBilledOnce) {
  const std::string k = "{qb3}:quota", req = "{qb3}:quota:req:r1";
  Del(k);
  Del(req);
  QuotaV2(k, 1, 1, "cap", 5);
  QuotaV2(k, 1, 2, "cap", 5, req);
  const auto again = QuotaV2(k, 1, 2, "cap", 5, req);
  EXPECT_EQ(again.ints[5], 1);  // replayed
  EXPECT_EQ(HGet(k, "bo"), "2");
}

TEST_F(LuaTest, ANewMonthFilesTheLastMonthsBilledOverage) {
  const std::string k = "{qb4}:quota";
  Del(k);
  const auto [month, month_left] = MonthOf(RedisNowMs());
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s bm %lld bu 7 bo 3", k.c_str(), month - 1));
  QuotaV2(k, 10, 1, "cap", 5);
  EXPECT_EQ(HGet(k, "closed:m" + std::to_string(month - 1)), "7,3");
  EXPECT_EQ(HGet(k, "bm"), std::to_string(month));
  EXPECT_EQ(HGet(k, "bu"), "1");
  EXPECT_EQ(HGet(k, "bo"), "0");
  const long long retain = 35LL * 86400000;
  EXPECT_NEAR(std::stod(HGet(k, "keep_until")), static_cast<double>(RedisNowMs() + retain), 2000.0);
  EXPECT_GT(Pttl(k), retain - 2000);  // kept for the billing job though this month bills nothing yet
}

TEST_F(LuaTest, AMonthWithoutBilledOverageIsNotFiledNorKept) {
  const std::string k = "{qb5}:quota";
  Del(k);
  const long long month = MonthOf(RedisNowMs()).first;
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s bm %lld bu 7 bo 0", k.c_str(), month - 1));
  QuotaV2(k, 10, 1, "cap", 5);
  EXPECT_EQ(HGet(k, "closed:m" + std::to_string(month - 1)), "");
  EXPECT_LE(Pttl(k), 86400000 + 3600000);  // a key with nothing to bill goes after its day, as before
}

TEST_F(LuaTest, BilledOverageKeepsTheCounterPastItsMonth) {
  const std::string k = "{qb6}:quota";
  Del(k);
  const long long month_left = MonthOf(RedisNowMs()).second;
  QuotaV2(k, 1, 1, "cap", 5);
  EXPECT_LE(Pttl(k), 86400000 + 3600000);
  QuotaV2(k, 1, 1, "cap", 5);  // billed overage
  EXPECT_GT(Pttl(k), month_left + 35LL * 86400000 - 2000);
}

TEST_F(LuaTest, NewUtcDayResetsOverageToo) {
  const std::string k = "{q11}:quota";
  Del(k);
  for (int i = 0; i < 5; ++i) QuotaV2(k, 3, 1, "cap", 2);
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s day 1", k.c_str()));  // yesterday's data
  const auto r = QuotaV2(k, 3, 1, "cap", 2);
  EXPECT_EQ(r.ints[1], 1);
  EXPECT_EQ(r.ints[4], 0);
  EXPECT_EQ(HGet(k, "overage_used"), "0");
}

TEST_F(LuaTest, RetryWithTheSameRequestIdIsCountedOnce) {
  const std::string k = "{q12}:quota", req = "{q12}:quota:req:abc";
  Del(k);
  Del(req);
  Del("{q12}:quota:req:other");  // stored answers outlive the test by 5 min
  const auto first = QuotaV2(k, 10, 1, "hard", 0, req);
  const auto retry = QuotaV2(k, 10, 1, "hard", 0, req);
  EXPECT_EQ(first.ints[0], 1);
  EXPECT_EQ(retry.ints[0], 1);
  EXPECT_EQ(first.ints[5], 0);  // replayed: usage events skip the retry
  EXPECT_EQ(retry.ints[5], 1);
  EXPECT_EQ(retry.ints[1], first.ints[1]);
  EXPECT_EQ(retry.ints[2], first.ints[2]);
  EXPECT_EQ(HGet(k, "used"), "1");
  EXPECT_EQ(QuotaV2(k, 10, 1, "hard", 0, "{q12}:quota:req:other").ints[1], 2);  // another id counts
  const long long ttl = Pttl(req);
  EXPECT_GT(ttl, 0);
  EXPECT_LE(ttl, 300000);
}

TEST_F(LuaTest, RetryOfADeniedRequestGetsTheSameDenial) {
  const std::string k = "{q13}:quota", req = "{q13}:quota:req:abc";
  Del(k);
  Del(req);
  SetUsedToday(k, 3);
  EXPECT_EQ(QuotaV2(k, 3, 1, "hard", 0, req).ints[0], 0);
  EXPECT_EQ(QuotaV2(k, 10, 1, "hard", 0, req).ints[0], 0);  // upgraded since, but it is the same request
  EXPECT_EQ(QuotaV2(k, 10, 1, "hard", 0).ints[0], 1);        // a new request sees the upgrade
}

// ---- return_lease.lua ----

TEST_F(LuaTest, ReturnedTokensGoBackToTheBucketButNeverAboveCapacity) {
  const std::string k = "{rl1}:tb";
  Del(k);
  for (int i = 0; i < 5; ++i) TB(k, 5, 0, 1);  // rate 0: nothing refills during the test
  EXPECT_EQ(TB(k, 5, 0, 1).ints[0], 0);
  EXPECT_EQ(Run(return_sha_, k, {"5", "2"}).ints[1], 2);
  EXPECT_EQ(TB(k, 5, 0, 1).ints[0], 1);
  EXPECT_EQ(Run(return_sha_, k, {"5", "100"}).ints[1], 5);  // clamped at the capacity
  const long long ttl = Pttl(k);
  EXPECT_GT(ttl, 0);  // returning keeps the bucket's TTL
}

TEST_F(LuaTest, ReturnToAMissingBucketCreatesNothing) {  // a missing bucket is already full
  const std::string k = "{rl2}:tb";
  Del(k);
  EXPECT_EQ(Run(return_sha_, k, {"5", "1"}).ints[1], 5);
  auto* r = static_cast<redisReply*>(redisCommand(ctx_.get(), "EXISTS %s", k.c_str()));
  EXPECT_EQ(r->integer, 0);
  freeReplyObject(r);
}

// ---- fixed window ----

constexpr int kHour = 3'600'000;  // long windows keep a boundary out of the deterministic tests

TEST_F(LuaTest, FixedWindowCountsUpToLimitThenDeniesUntilWindowEnds) {
  const std::string k = "{f1}:fw";
  Del(k);
  for (int i = 2; i >= 0; --i) {
    auto r = FW(k, 3, kHour);
    ASSERT_FALSE(r.error) << r.err;
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
  }
  auto d = FW(k, 3, kHour);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_GT(d.ints[2], 0);
  EXPECT_LE(d.ints[2], kHour);      // retry when this window ends
  EXPECT_LE(Pttl(k), kHour);        // and the key goes away then too
}

TEST_F(LuaTest, FixedWindowStartsFromZeroInANewWindow) {
  const std::string k = "{f2}:fw";
  Del(k);
  for (int i = 0; i < 3; ++i) FW(k, 3, kHour);
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s win 1", k.c_str()));  // pretend it is an old window
  auto r = FW(k, 3, kHour);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 2);
}

TEST_F(LuaTest, FixedWindowClockBackwardsKeepsTheCount) {
  const std::string k = "{f3}:fw";
  Del(k);
  for (int i = 0; i < 3; ++i) FW(k, 3, kHour);
  const long long next_win = RedisNowMs() / kHour + 1;  // state written by a clock one window ahead
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s win %lld", k.c_str(), next_win));
  EXPECT_EQ(FW(k, 3, kHour).ints[0], 0);
}

TEST_F(LuaTest, FixedWindowRequestLargerThanLimitNeverPasses) {
  const std::string k = "{f4}:fw";
  Del(k);
  auto r = FW(k, 3, kHour, 4);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[1], 3);          // nothing consumed
  EXPECT_EQ(r.ints[2], 0);          // waiting would not help
}

// ---- sliding window ----

TEST_F(LuaTest, SlidingWindowCountsUpToLimitThenDenies) {
  const std::string k = "{s1}:sw";
  Del(k);
  for (int i = 2; i >= 0; --i) {
    auto r = SW(k, 3, kHour);
    ASSERT_FALSE(r.error) << r.err;
    EXPECT_EQ(r.ints[0], 1);
    EXPECT_EQ(r.ints[1], i);
  }
  auto d = SW(k, 3, kHour);
  EXPECT_EQ(d.ints[0], 0);
  EXPECT_GT(d.ints[2], 0);
  EXPECT_LE(Pttl(k), 2LL * kHour);  // kept through the next window, where it still counts
}

TEST_F(LuaTest, SlidingWindowForgetsWindowsOlderThanThePreviousOne) {
  const std::string k = "{s2}:sw";
  Del(k);
  for (int i = 0; i < 3; ++i) SW(k, 3, kHour);
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s win 1", k.c_str()));
  auto r = SW(k, 3, kHour);
  EXPECT_EQ(r.ints[0], 1);
  EXPECT_EQ(r.ints[1], 2);
}

TEST_F(LuaTest, SlidingWindowClockBackwardsKeepsTheCount) {
  const std::string k = "{s3}:sw";
  Del(k);
  for (int i = 0; i < 3; ++i) SW(k, 3, kHour);
  const long long next_win = RedisNowMs() / kHour + 1;  // state written by a clock one window ahead
  freeReplyObject(redisCommand(ctx_.get(), "HSET %s win %lld", k.c_str(), next_win));
  EXPECT_EQ(SW(k, 3, kHour).ints[0], 0);
}

TEST_F(LuaTest, SlidingWindowRequestLargerThanLimitNeverPasses) {
  const std::string k = "{s4}:sw";
  Del(k);
  auto r = SW(k, 3, kHour, 4);
  EXPECT_EQ(r.ints[0], 0);
  EXPECT_EQ(r.ints[2], 0);
}

// The defining difference: `limit` at the end of one window plus `limit` at the start of the
// next. Fixed window admits both (2x in ~150 ms); sliding window still counts the first batch.
TEST_F(LuaTest, WindowEdgeBurstFixedAdmitsTwiceTheLimitSlidingDoesNot) {
  const std::string f = "{e1}:fw", s = "{e1}:sw";
  Del(f);
  Del(s);
  constexpr int kWindow = 1000, kLimit = 10;
  StartClockCheck();
  SleepPastNextBoundary(kWindow, kWindow - 150);  // 150 ms before a boundary
  for (int i = 0; i < kLimit; ++i) {
    ASSERT_EQ(FW(f, kLimit, kWindow).ints[0], 1);
    ASSERT_EQ(SW(s, kLimit, kWindow).ints[0], 1);
  }
  SleepPastNextBoundary(kWindow, 20);             // just after it
  int fixed = 0, sliding = 0;
  for (int i = 0; i < kLimit; ++i) {
    fixed += static_cast<int>(FW(f, kLimit, kWindow).ints[0]);
    sliding += static_cast<int>(SW(s, kLimit, kWindow).ints[0]);
  }
  if (std::abs(ClockStepMs()) > 10) GTEST_SKIP() << "wall clock stepped " << ClockStepMs() << " ms";
  EXPECT_EQ(fixed, kLimit);
  EXPECT_LE(sliding, 1);
}

TEST_F(LuaTest, SlidingWindowRetryAfterWhenCurrentWindowIsFull) {
  const std::string k = "{r1}:sw";
  Del(k);
  StartClockCheck();
  SleepPastNextBoundary(1000, 100);
  for (int i = 0; i < 5; ++i) SW(k, 5, 1000);
  ExpectRetryAfter(k);  // only fits in the next window, once this one's weight has decayed
}

TEST_F(LuaTest, SlidingWindowRetryAfterWhenPreviousWindowIsInTheWay) {
  const std::string k = "{r2}:sw";
  Del(k);
  StartClockCheck();
  SleepPastNextBoundary(1000, 900);
  for (int i = 0; i < 5; ++i) SW(k, 5, 1000);
  SleepPastNextBoundary(1000, 20);
  ExpectRetryAfter(k);  // current window empty; waits for the previous one to decay
}

}  // namespace
