// Property-based tests (Week 6, docs/THESIS_PLAN.md). Random histories of customer checks, retries,
// plan changes, Redis outages, clock jumps and UTC day rollovers run through the real engine
// (CompositeCore over StrictCore or LeasedCore, PlanStore, DenyCache) on a fake Redis that executes
// the scripts' rules (lua/token_bucket.lua, quota.lua, return_lease.lua) under a fake clock.
//
// 1. Exact plans: every answer, every usage event and every alert must equal a reference model that
//    states the product rules of Chapter 3, 3.4 and 3.9 in one sequential function, written from those
//    rules: daily and monthly periods, boosts, prepaid top-ups, overage policies, alerts.
// 2. Leased quota (plans of 100,000 a day or more) admits differently from the exact path by design,
//    so it is checked against its bounds instead (3.4.5): Redis never grants beyond the plan, and
//    what N instances admitted is what Redis granted minus at most N x C units still held in leases.
//
// The scripts' arithmetic itself is tested against a real Redis (lua_scripts_test.cc). A failing seed
// prints itself; replay it with RL_PROPERTY_SEED=<seed>. RL_PROPERTY_SEEDS sets how many seeds run
// per configuration (default 300).
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "core/composite_core.h"
#include "core/leased_core.h"
#include "core/strict_core.h"
#include "lease/chunk_policy.h"
#include "lease/deny_cache.h"
#include "plan/plan_store.h"
#include "redis/keys.h"
#include "server/merchants.h"

namespace ratelimiter {
namespace {

std::int64_t g_now_ns = 0;
std::int64_t FakeNow() { return g_now_ns; }
std::int64_t NowMs() { return g_now_ns / 1'000'000; }
void Advance(std::int64_t ms) { g_now_ns += ms * 1'000'000; }
constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kIdTtlMs = 300'000;

// The period a quota counts in at `now`: the UTC day, or the UTC calendar month numbered as quota.lua
// does (year * 12 + month - 1), with the ms until it ends and its name ("d<day>" or "m<month>").
struct Period {
  bool monthly;
  std::int64_t id, reset_after_ms;
  std::string name;
};
// A month anchored on day `anchor` (P5) runs from that day to the same day of the next month, and is
// numbered by the calendar month it starts in; anchor 1 is the calendar month.
Period PeriodOf(bool monthly, std::int64_t now, unsigned anchor = 1) {
  if (!monthly) return {false, now / kDayMs, (now / kDayMs + 1) * kDayMs - now, "d" + std::to_string(now / kDayMs)};
  using namespace std::chrono;
  const sys_days day{floor<days>(sys_time<milliseconds>(milliseconds(now)))};
  const year_month_day ymd{day};
  year_month start = ymd.year() / ymd.month();
  if (static_cast<unsigned>(ymd.day()) < anchor) start -= months(1);
  const sys_days next{(start + months(1)) / std::chrono::day(anchor)};
  const std::int64_t id = static_cast<int>(start.year()) * 12LL + static_cast<unsigned>(start.month()) - 1;
  return {true, id, duration_cast<milliseconds>(next.time_since_epoch()).count() - now, "m" + std::to_string(id)};
}

// One message on rl-alerts.
struct Alert {
  std::string customer, event;
  std::int64_t used, over, limit;
  std::string period;
  bool operator==(const Alert&) const = default;
};

using Fields = std::unordered_map<std::string, std::string>;

// ---- A fake Redis that runs the scripts' rules (transcribed from lua/) on the fake clock ----

class ScriptRedis final : public RedisBackend {
 public:
  bool down = false;                   // every call fails, as with a dead master
  std::map<std::string, Fields> hashes;  // plan hashes, by key
  std::vector<Alert> alerts;           // published on rl-alerts

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view request_id,
            ScriptCallback done) override {
    ScriptResult r;
    if (down) {
      r.redis_error = true;
    } else if (rule.algorithm == Algorithm::kTokenBucket) {
      r = Bucket(rule, std::string(key), requested);
    } else if (rule.algorithm == Algorithm::kQuota) {
      r = Quota(rule, std::string(key), requested, std::string(request_id));
    } else {
      ADD_FAILURE() << "unexpected algorithm";
    }
    done(r);
  }

  // lua/return_lease.lua: adds the tokens back, never above capacity; a missing bucket is full.
  void Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) override {
    ScriptResult r;
    if (down) {
      r.redis_error = true;
      done(r);
      return;
    }
    r.allowed = true;
    const auto it = buckets_.find(rule.name + "|" + std::string(key));
    if (it == buckets_.end() || NowMs() > it->second.expires_ms) {
      r.remaining = rule.limit;
    } else {
      it->second.tokens_m = std::min(rule.limit * 1000.0, it->second.tokens_m + tokens * 1000.0);
      r.remaining = static_cast<std::uint32_t>(std::floor(it->second.tokens_m / 1000));
    }
    done(r);
  }

  void HGetAll(std::string key, HashCallback done) override {
    HashResult h;
    if (down) {
      h.redis_error = true;
    } else if (const auto it = hashes.find(key); it != hashes.end()) {
      h.fields = it->second;
    }
    done(h);
  }

  // The billing totals of `rule|key` for a calendar month, as the billing job reads them: the
  // counter's own month, or the month it filed when a new one began. Key lifetimes are not modelled
  // (lua_scripts_test.cc checks them against Redis).
  std::optional<std::pair<std::int64_t, std::int64_t>> Billed(const std::string& counter, std::int64_t month) const {
    const auto it = billing_.find(counter);
    if (it == billing_.end()) return std::nullopt;
    if (it->second.bm == month) return std::pair{it->second.bu, it->second.bo};
    if (const auto c = it->second.closed.find(month); c != it->second.closed.end()) return c->second;
    return std::nullopt;
  }
  std::int64_t BillingMonth(const std::string& counter) const {
    return billing_.count(counter) ? billing_.at(counter).bm : -1;
  }
  std::vector<std::int64_t> FiledMonths(const std::string& counter) const {
    std::vector<std::int64_t> out;
    if (billing_.count(counter)) {
      for (const auto& [month, totals] : billing_.at(counter).closed) out.push_back(month);
    }
    return out;
  }

  // Units the quota counter `rule|key` granted per UTC day: {day: (used, overage_used)}.
  std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> Granted(const std::string& counter) const {
    auto out = history_.count(counter) ? history_.at(counter) : decltype(history_)::mapped_type{};
    if (const auto it = counters_.find(counter); it != counters_.end() && !it->second.monthly) {
      out[it->second.period] = {it->second.used, it->second.over};
    }
    return out;
  }

 private:
  struct BucketState {
    double tokens_m;
    std::int64_t last_ms, expires_ms;
  };
  struct CounterState {
    bool monthly;
    std::int64_t period, used, over, alerts;
  };
  struct BillingState {  // the same hash's fields bm, bu, bo and closed:m<month>
    std::int64_t bm = -1, bu = 0, bo = 0;
    std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> closed;
  };
  struct StoredAnswer {
    bool allowed;
    std::int64_t remaining;
    bool overage;
    std::int64_t expires_ms;
  };

  // lua/token_bucket.lua. Keys expire `ceil(capacity / rate s) + 1 s` after their last call, as
  // RedisPPBackend sets them; Redis treats a key as gone once now > its expiry.
  ScriptResult Bucket(const Rule& rule, const std::string& key, std::uint32_t requested) {
    const std::int64_t now = NowMs();
    const double cap_m = rule.limit * 1000.0, rate = rule.refill_per_sec;
    double tokens = cap_m;
    std::int64_t last = now;
    if (const auto it = buckets_.find(rule.name + "|" + key); it != buckets_.end() && now <= it->second.expires_ms) {
      tokens = it->second.tokens_m;
      last = it->second.last_ms;
    }
    tokens = std::min(cap_m, tokens + static_cast<double>(std::max<std::int64_t>(0, now - last)) * rate);
    ScriptResult r;
    const double need = requested * 1000.0;
    if (need <= tokens) {
      tokens -= need;
      r.allowed = true;
    } else if (need <= cap_m && rate > 0) {
      r.retry_after_ms = static_cast<std::uint32_t>(std::ceil((need - tokens) / rate));
    }
    const double full_after_ms = rate > 0 ? rule.limit / rate * 1000.0 : static_cast<double>(kDayMs);
    buckets_[rule.name + "|" + key] = {tokens, now, now + static_cast<std::int64_t>(std::ceil(full_after_ms)) + 1000};
    r.remaining = static_cast<std::uint32_t>(std::floor(tokens / 1000));
    return r;
  }

  // lua/quota.lua.
  ScriptResult Quota(const Rule& rule, const std::string& key, std::uint32_t requested, const std::string& id) {
    const std::int64_t now = NowMs(), cost = requested, extra = rule.overage_extra;
    const Period p = PeriodOf(rule.quota_period == QuotaPeriod::kMonth, now, rule.anchor);
    std::int64_t limit = rule.limit;
    if (now < rule.boost_until_ms) limit += rule.boost;
    if (rule.topup > 0 && rule.topup_period == p.name) limit += rule.topup;
    ScriptResult r;
    r.reset_after_ms = static_cast<std::uint32_t>(p.reset_after_ms);
    r.limit = static_cast<std::uint32_t>(limit);
    const std::string counter = rule.name + "|" + key;
    if (!id.empty()) {
      if (const auto it = answers_.find(counter + "|" + id); it != answers_.end() && now <= it->second.expires_ms) {
        r.allowed = it->second.allowed;
        r.remaining = static_cast<std::uint32_t>(it->second.remaining);
        r.overage = it->second.overage;
        r.replayed = true;
        r.retry_after_ms = r.allowed ? 0 : r.reset_after_ms;
        return r;
      }
    }
    CounterState& c = counters_.try_emplace(counter, CounterState{p.monthly, p.id, 0, 0, 0}).first->second;
    if (c.monthly != p.monthly || c.period != p.id) {  // a new period, or the other kind of period
      if (!c.monthly) history_[counter][c.period] = {c.used, c.over};
      c = {p.monthly, p.id, 0, 0, 0};
    }
    BillingState& b = billing_[counter];
    if (const std::int64_t month = std::max(PeriodOf(true, now, rule.anchor).id, b.bm); b.bm != month) {
      if (b.bm >= 0 && b.bo > 0) b.closed[b.bm] = {b.bu, b.bo};
      b.bm = month;
      b.bu = b.bo = 0;
    }
    bool allowed = false, overage = false;
    if (c.used + cost <= limit) {
      c.used += cost;
      b.bu += cost;
      allowed = true;
    } else if (rule.overage == Overage::kGrace) {
      if (c.used + cost <= limit + extra) {
        c.used += cost;
        b.bu += cost;
        allowed = true;
      }
    } else if (rule.overage == Overage::kCap) {
      const std::int64_t in_plan = std::max<std::int64_t>(0, std::min(cost, limit - c.used));
      if (c.over + cost - in_plan <= extra) {
        c.used += in_plan;
        c.over += cost - in_plan;
        b.bu += in_plan;
        b.bo += cost - in_plan;
        allowed = overage = true;
      }
    }
    const auto alert = [&](std::int64_t bit, const char* event) {
      if ((c.alerts & bit) != 0) return;
      c.alerts |= bit;
      alerts.push_back({key, event, c.used, c.over, limit, p.name});
    };
    if (limit > 0 && c.used * 5 >= limit * 4) alert(1, "quota.80");
    if (limit > 0 && c.used >= limit) alert(2, "quota.100");
    if (c.over > 0) alert(4, "overage.started");
    if (!allowed) alert(8, "quota.denied");
    const std::int64_t remaining = std::max<std::int64_t>(0, limit - c.used);
    if (!id.empty()) answers_[counter + "|" + id] = {allowed, remaining, overage, now + kIdTtlMs};
    r.allowed = allowed;
    r.overage = overage;
    r.remaining = static_cast<std::uint32_t>(remaining);
    r.retry_after_ms = allowed ? 0 : r.reset_after_ms;
    return r;
  }

  std::unordered_map<std::string, BucketState> buckets_;
  std::unordered_map<std::string, CounterState> counters_;
  std::unordered_map<std::string, BillingState> billing_;
  std::map<std::string, std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>>> history_;
  std::unordered_map<std::string, StoredAnswer> answers_;
};

RuleTable CustomerRules() {
  RuleTable t;
  t.emplace("burst", Rule{.name = "burst", .algorithm = Algorithm::kTokenBucket, .strict = true,
                          .limit_from = PlanLimit::kBurst});
  t.emplace("daily_quota", Rule{.name = "daily_quota", .algorithm = Algorithm::kQuota, .strict = true,
                                .limit_from = PlanLimit::kDaily, .quota_auto = true});
  t.emplace("customer", Rule{.name = "customer", .burst_rule = "burst", .quota_rule = "daily_quota"});
  return t;
}

// ---- The reference model: the product rules of Chapter 3, 3.4, one customer call at a time ----

struct ModelPlan {  // a missing hash or field means Free (3.1.3)
  std::uint32_t daily = 200, burst = 2, refill = 2;
  Overage overage = Overage::kHard;
  std::uint32_t extra = 0;
  bool suspended = false, fail_open = false;
  bool monthly = false;                    // the quota is per UTC calendar month (3.9.3)
  std::uint32_t boost = 0;                 // extra units while now < boost_until_ms
  std::int64_t boost_until_ms = 0;
  std::uint32_t topup = 0;                 // prepaid units for the period named topup_period only
  std::string topup_period;
  unsigned anchor = 1;                     // the day a month starts (P5); 1: the calendar month
};

struct Usage {
  std::string customer;
  Reason reason;
  std::uint32_t cost;
  bool operator==(const Usage&) const = default;
};

class Model {
 public:
  explicit Model(bool plan_cache) : plan_cache_(plan_cache) {}

  bool down = false;
  std::map<std::string, ModelPlan> plans;  // what Redis holds; absent = Free
  std::vector<Usage> usage;
  std::vector<Alert> alerts;  // what the quota script published
  // What each UTC calendar month admitted, per customer, for its invoice (3.9, P3): units within
  // the plan or free grace, and billed overage units. A replayed answer adds nothing.
  std::map<std::string, std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>>> billed;

  // A plan-change message arrived (only with the plan cache, as PlanListener does it).
  void Announce(const std::string& c) { denial_.erase(c); }

  CheckResult Check(const std::string& c, std::uint32_t cost, const std::string& id) {
    const CheckResult r = Decide(c, cost, id);
    if (!r.replayed) usage.push_back({c, r.reason, cost});
    return r;
  }

 private:
  struct Bucket {
    double tokens_m;
    std::int64_t last_ms, expires_ms;
  };
  std::map<std::string, std::int64_t> billing_month_;  // the latest billing month each customer counted in
  struct Counter {  // one customer's quota in its current period
    bool monthly = false;
    std::int64_t period = -1, used = 0, over = 0;
    std::uint32_t alerted = 0;  // 1: 80%, 2: the limit, 4: overage, 8: a denial; once a period
  };
  struct Answer {
    bool allowed;
    std::int64_t remaining;
    bool overage;
    std::int64_t expires_ms;
  };
  using QuotaTerms =
      std::tuple<std::uint32_t, std::uint32_t, Overage, bool, std::uint32_t, std::int64_t, std::uint32_t, std::string, unsigned>;
  struct Denial {
    std::int64_t until_ms;
    QuotaTerms terms;  // a denial holds only under its plan's quota terms
  };

  static QuotaTerms Terms(const ModelPlan& p) {
    return {p.daily, p.extra, p.overage, p.monthly, p.boost, p.boost_until_ms, p.topup, p.topup_period, p.anchor};
  }

  // Redis cannot answer: a paid plan admits unverified, anything else is unavailable for a second.
  static CheckResult Unreachable(const ModelPlan& p) {
    CheckResult r;
    r.allowed = p.fail_open;
    r.reason = p.fail_open ? Reason::kUnverified : Reason::kUnavailable;
    r.limit = p.daily;
    r.retry_after_ms = p.fail_open ? 0 : 1000;
    return r;
  }

  CheckResult Decide(const std::string& c, std::uint32_t cost, const std::string& id) {
    const std::int64_t now = NowMs();
    // The plan: read from Redis; during an outage the last one read, if the cache keeps one.
    ModelPlan plan;
    if (!down) {
      plan = plans.count(c) ? plans[c] : ModelPlan{};
      last_read_[c] = plan;
    } else if (plan_cache_ && last_read_.count(c)) {
      plan = last_read_[c];
    } else {
      return Unreachable(ModelPlan{});  // an unknown customer is Free
    }
    // 1. A suspended account is denied; waiting does not help.
    if (plan.suspended) {
      CheckResult r;
      r.reason = Reason::kSuspended;
      return r;
    }
    // 2. A customer this instance saw run out of quota is denied until the reset, under the same terms.
    if (const auto it = denial_.find(c); it != denial_.end()) {
      if (now < it->second.until_ms && it->second.terms == Terms(plan)) {
        CheckResult r;
        r.reason = Reason::kQuotaExhausted;
        r.limit = plan.daily;
        r.retry_after_ms = r.reset_after_ms = static_cast<std::uint32_t>(it->second.until_ms - now);
        return r;
      }
      denial_.erase(it);
    }
    if (down) return Unreachable(plan);
    // 3. Burst: a token bucket of plan.burst tokens refilled at plan.refill a second, in
    // thousandths. An idle bucket is forgotten once it would be full again (+1 s) and restarts full.
    const double cap_m = plan.burst * 1000.0, rate = plan.refill;
    auto b = bucket_.find(c);
    if (b == bucket_.end() || now > b->second.expires_ms) b = bucket_.insert_or_assign(c, Bucket{cap_m, now, 0}).first;
    double& tokens = b->second.tokens_m;
    tokens = std::min(cap_m, tokens + static_cast<double>(std::max<std::int64_t>(0, now - b->second.last_ms)) * rate);
    b->second.last_ms = now;
    b->second.expires_ms = now + static_cast<std::int64_t>(std::ceil(plan.burst / rate * 1000.0)) + 1000;
    if (cost * 1000.0 > tokens) {
      CheckResult r;
      r.reason = Reason::kRateLimited;
      r.limit = plan.burst;
      r.remaining = static_cast<std::uint32_t>(std::floor(tokens / 1000));
      r.retry_after_ms = cost * 1000.0 <= cap_m ? static_cast<std::uint32_t>(std::ceil((cost * 1000.0 - tokens) / rate)) : 0;
      return r;
    }
    tokens -= cost * 1000.0;
    // 4. The quota of the current period (a UTC day, or a UTC calendar month) under the plan's overage
    // policy. The limit in force adds a boost until it ends and a top-up bought for this period. A
    // request id decided in the last five minutes gets its first answer again and is not counted.
    const Period period = PeriodOf(plan.monthly, now, plan.anchor);
    std::int64_t limit = plan.daily;
    if (now < plan.boost_until_ms) limit += plan.boost;
    if (plan.topup > 0 && plan.topup_period == period.name) limit += plan.topup;
    CheckResult r;
    r.limit = static_cast<std::uint32_t>(limit);
    r.reset_after_ms = static_cast<std::uint32_t>(period.reset_after_ms);
    Answer a{};
    if (const auto it = answers_.find(c + "|" + id); !id.empty() && it != answers_.end() && now <= it->second.expires_ms) {
      a = it->second;
      r.replayed = true;
    } else {
      Counter& d = counters_[c];
      if (d.monthly != period.monthly || d.period != period.id) d = {period.monthly, period.id, 0, 0, 0};
      const std::int64_t room = limit - d.used;
      // A customer's billing month never goes back (P5), even if its anchor day changes.
      std::int64_t& month = billing_month_[c];
      month = std::max(month, PeriodOf(true, now, plan.anchor).id);
      auto& [in_plan, billed_over] = billed[c][month];
      if (cost <= room) {  // within the plan, whatever the policy
        d.used += cost;
        in_plan += cost;
        a.allowed = true;
      } else if (plan.overage == Overage::kGrace && cost <= room + plan.extra) {  // free extra units
        d.used += cost;
        in_plan += cost;
        a.allowed = true;
      } else if (plan.overage == Overage::kCap) {  // billed beyond the plan, up to the cap
        const std::int64_t inside = std::clamp<std::int64_t>(room, 0, cost);
        if (d.over + (cost - inside) <= plan.extra) {
          d.used += inside;
          d.over += cost - inside;
          in_plan += inside;
          billed_over += cost - inside;
          a.allowed = a.overage = true;
        }
      }
      // Alerts: each threshold the first time this period, against the limit in force.
      const auto alert = [&](std::uint32_t bit, const char* event) {
        if ((d.alerted & bit) != 0) return;
        d.alerted |= bit;
        alerts.push_back({c, event, d.used, d.over, limit, period.name});
      };
      if (limit > 0 && 5 * d.used >= 4 * limit) alert(1, "quota.80");
      if (limit > 0 && d.used >= limit) alert(2, "quota.100");
      if (d.over > 0) alert(4, "overage.started");
      if (!a.allowed) alert(8, "quota.denied");
      a.remaining = std::max<std::int64_t>(0, limit - d.used);
      a.expires_ms = now + kIdTtlMs;
      if (!id.empty()) answers_[c + "|" + id] = a;
    }
    r.allowed = a.allowed;
    r.remaining = static_cast<std::uint32_t>(a.remaining);
    r.reason = !a.allowed ? Reason::kQuotaExhausted : a.overage ? Reason::kOverage : Reason::kAllowed;
    if (!a.allowed) {
      r.retry_after_ms = r.reset_after_ms;
      tokens = std::min(cap_m, tokens + cost * 1000.0);  // the burst token goes back
      if (!r.replayed) denial_[c] = {now + r.reset_after_ms, Terms(plan)};
    }
    return r;
  }

  const bool plan_cache_;
  std::map<std::string, ModelPlan> last_read_;
  std::map<std::string, Bucket> bucket_;
  std::map<std::string, Counter> counters_;
  std::map<std::string, Answer> answers_;
  std::map<std::string, Denial> denial_;
};

// ---- Histories ----

std::string Show(const CheckResult& r) {
  char buf[200];
  std::snprintf(buf, sizeof buf, "{allowed %d reason %d remaining %u retry %u limit %u reset %u replayed %d}",
                r.allowed, static_cast<int>(r.reason), r.remaining, r.retry_after_ms, r.limit, r.reset_after_ms,
                r.replayed);
  return buf;
}

bool Same(const CheckResult& a, const CheckResult& b) {
  return a.allowed == b.allowed && a.reason == b.reason && a.remaining == b.remaining &&
         a.retry_after_ms == b.retry_after_ms && a.limit == b.limit && a.reset_after_ms == b.reset_after_ms &&
         a.replayed == b.replayed;
}

// A random plan, as the admin tool might write it (small numbers, so a history reaches every limit),
// and what it means.
std::pair<std::optional<Fields>, ModelPlan> RandomPlan(std::mt19937_64& rng) {
  const auto pick = [&rng](std::uint32_t lo, std::uint32_t hi) {
    return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
  };
  ModelPlan m;
  switch (pick(0, 9)) {
    case 0:
      m.suspended = true;
      return {Fields{{"plan", "suspended"}, {"daily", "0"}, {"burst", "0"}, {"refill", "0"}}, m};
    case 1:
      return {std::nullopt, m};  // the hash is deleted: Free
    case 2:
      m.daily = pick(3, 40);  // only some fields: the rest stay Free
      return {Fields{{"plan", "pay"}, {"daily", std::to_string(m.daily)}}, m};
    default:
      break;
  }
  m.daily = pick(3, 40);
  m.burst = pick(1, 15);
  m.refill = pick(1, 5);
  m.overage = static_cast<Overage>(pick(0, 2));
  m.extra = pick(0, 15);
  m.fail_open = pick(0, 1) == 1;
  static constexpr const char* kPolicy[] = {"hard", "grace", "cap"};
  Fields f{{"plan", "pay"},
           {"daily", std::to_string(m.daily)},
           {"burst", std::to_string(m.burst)},
           {"refill", std::to_string(m.refill)},
           {"overage", kPolicy[static_cast<int>(m.overage)]},
           {"extra", std::to_string(m.extra)},
           {"failure", m.fail_open ? "open" : "closed"}};
  // Phase 2 terms (3.9): a monthly period, a boost (some already over), a top-up (some bought for
  // another period, some with a malformed period, which gives no top-up at all).
  if (pick(0, 6) == 0) {
    m.monthly = true;
    f["period"] = "month";
  }
  if (pick(0, 3) == 0) {  // billed from a subscription day (P5), near today so histories cross it
    using namespace std::chrono;
    const unsigned today = static_cast<unsigned>(
        year_month_day{floor<days>(sys_time<milliseconds>(milliseconds(NowMs())))}.day());
    const unsigned day = std::min(28u, today + pick(0, 2));
    m.anchor = day;
    f["anchor"] = std::to_string(day);
  } else if (pick(0, 9) == 0) {
    f["anchor"] = pick(0, 1) ? "31" : "0";  // out of range: the calendar month
  }
  if (pick(0, 4) == 0) {
    m.boost = pick(1, 10);
    m.boost_until_ms = NowMs() + std::uniform_int_distribution<std::int64_t>(-60'000, 3'600'000)(rng);
    f["boost"] = std::to_string(m.boost);
    f["boost_until"] = std::to_string(m.boost_until_ms);
  }
  if (pick(0, 4) == 0) {
    const std::uint32_t units = pick(1, 10);
    const Period now = PeriodOf(m.monthly, NowMs(), m.anchor);
    const int which = static_cast<int>(pick(0, 9));
    const std::string period = which < 7 ? now.name : which < 9 ? (now.monthly ? "m" : "d") + std::to_string(now.id - 1) : "x1";
    if (period != "x1") {
      m.topup = units;
      m.topup_period = period;
    }
    f["topup"] = std::to_string(units);
    f["topup_period"] = period;
  }
  return {f, m};
}

std::int64_t RandomStep(std::mt19937_64& rng) {  // how far the clock moves, in ms
  const int kind = std::uniform_int_distribution<int>(0, 99)(rng);
  const auto ms = [&rng](std::int64_t lo, std::int64_t hi) { return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng); };
  if (kind < 45) return ms(0, 100);
  if (kind < 75) return ms(100, 3000);
  if (kind < 90) return ms(3000, 60'000);
  if (kind < 95) return ms(60'000, 600'000);  // past a stored answer's five minutes
  if (kind < 97) return kIdTtlMs + ms(-1, 1);  // a retry at the edge of the five minutes
  const std::int64_t to_midnight = kDayMs - NowMs() % kDayMs;
  if (kind < 98) return std::max<std::int64_t>(0, to_midnight + ms(-1500, 1500));  // around the reset
  if (kind < 99) return to_midnight + ms(0, 3 * 3'600'000);  // into the next UTC day
  // Around the end of the month, so monthly quotas roll over too.
  return std::max<std::int64_t>(0, PeriodOf(true, NowMs()).reset_after_ms + ms(-1500, 1500));
}

// What the histories reached, summed over every seed of a run: a pass only means something if every
// branch was taken.
struct Coverage {
  std::map<Reason, std::uint64_t> reasons;
  std::map<std::string, std::uint64_t> alerts;
  std::uint64_t checks = 0, replayed = 0, plan_changes = 0, outages = 0, rollovers = 0;
  std::uint64_t month_rollovers = 0, monthly_checks = 0, boosted_checks = 0, topped_up_checks = 0;
  std::uint64_t anchored_checks = 0, anchored_rollovers = 0;  // plans billed from a subscription day (P5)
  std::uint64_t billed_months = 0, filed_months = 0;  // months with billed overage; months filed by a rollover
};
Coverage g_coverage;
struct LeasedCoverage {
  std::uint64_t seeds = 0, days = 0, days_at_limit = 0, admitted = 0, held = 0;
} g_leased;

struct Config {
  bool plan_cache;  // PlanStore with a 5 s TTL and plan-change messages; else no cache, no messages
  bool leased;      // CompositeCore over LeasedCore (main.cc's default engine) instead of StrictCore
};

// One history: returns "" or what went wrong.
std::string RunExact(std::uint64_t seed, Config cfg) {
  std::mt19937_64 rng(seed);
  g_now_ns = (20'000 * kDayMs + std::uniform_int_distribution<std::int64_t>(0, kDayMs - 1)(rng)) * 1'000'000;
  ScriptRedis redis;
  PlanStore plans(redis, std::chrono::milliseconds(cfg.plan_cache ? 5000 : 0), &FakeNow);
  DenyCache deny;
  const RuleTable rules = CustomerRules();
  StrictCore customer_strict(redis, plans, rules);
  std::shared_ptr<RateLimiterCore> inner;
  LeasedCore* leased = nullptr;
  if (cfg.leased) {
    auto l = std::make_shared<LeasedCore>(redis, plans, rules, 1024, &FakeNow);
    leased = l.get();
    inner = std::move(l);
  } else {
    inner = std::make_shared<StrictCore>(redis, plans, rules);
  }
  std::vector<Usage> usage;
  CompositeCore core(inner, customer_strict, plans, deny, rules, &FakeNow,
                     [&usage](std::string_view c, Reason r, std::uint32_t cost) { usage.push_back({std::string(c), r, cost}); },
                     leased);
  Model model(cfg.plan_cache);
  std::map<std::string, unsigned> anchors;  // each customer's subscription day, fixed by its first plan (P5)

  const std::vector<std::string> customers{"cust:a", "cust:b", "cust:c"};
  std::map<std::string, std::vector<std::pair<std::string, std::uint32_t>>> sent;  // ids and costs, per customer
  std::deque<std::string> recent;  // the last steps, for the failure message
  const int steps = std::uniform_int_distribution<int>(200, 600)(rng);
  for (int step = 0; step < steps; ++step) {
    // Outages are short and rare: one starts in 2% of steps and ends in 20% of the steps it lasts.
    int op = std::uniform_int_distribution<int>(0, 99)(rng);
    if (redis.down && std::uniform_int_distribution<int>(0, 4)(rng) == 0) op = 99;
    else if (op >= 98 && redis.down) op = std::uniform_int_distribution<int>(0, 97)(rng);
    const std::string& c = customers[std::uniform_int_distribution<std::size_t>(0, customers.size() - 1)(rng)];
    std::string what;
    if (op < 72) {  // a check: new, or a retry of an earlier request id
      std::uint32_t cost = std::uniform_int_distribution<std::uint32_t>(1, 3)(rng);
      if (std::uniform_int_distribution<int>(0, 19)(rng) == 0) cost = std::uniform_int_distribution<std::uint32_t>(4, 12)(rng);
      std::string id;
      if (op < 50 || sent[c].empty()) {
        if (std::uniform_int_distribution<int>(0, 9)(rng) < 7) {
          id = "r" + std::to_string(step);
          sent[c].push_back({id, cost});
        }
      } else {
        // Most retries follow their request closely; some come back much later.
        const std::size_t n = sent[c].size();
        const std::size_t back = std::uniform_int_distribution<int>(0, 4)(rng) != 0
                                     ? std::uniform_int_distribution<std::size_t>(1, std::min<std::size_t>(n, 3))(rng)
                                     : std::uniform_int_distribution<std::size_t>(1, n)(rng);
        const auto& [old_id, old_cost] = sent[c][n - back];
        id = old_id;
        if (std::uniform_int_distribution<int>(0, 9)(rng) != 0) cost = old_cost;  // a true retry; else an id reused
      }
      what = "check " + c + " cost " + std::to_string(cost) + (id.empty() ? "" : " id " + id);
      std::optional<CheckResult> got;
      core.Check(CheckRequest{c, "customer", cost, id}, [&got](const CheckResult& r) { got = r; });
      const CheckResult want = model.Check(c, cost, id);
      if (!got) return "step " + std::to_string(step) + " (" + what + "): no answer";
      if (!Same(*got, want) || usage != model.usage || redis.alerts != model.alerts) {
        std::string trail;
        for (const auto& s : recent) trail += "\n    " + s;
        std::string alerts;
        if (redis.alerts != model.alerts) {
          alerts = " (alerts differ: script " + std::to_string(redis.alerts.size()) + ", model " +
                   std::to_string(model.alerts.size());
          if (!redis.alerts.empty()) alerts += ", script's last " + redis.alerts.back().event;
          if (!model.alerts.empty()) alerts += ", model's last " + model.alerts.back().event;
          alerts += ")";
        }
        return "step " + std::to_string(step) + " (" + what + "): got " + Show(*got) + ", model " + Show(want) +
               (usage != model.usage ? " (usage events differ)" : "") + alerts + "\n  last steps:" + trail;
      }
      what += " -> " + Show(want);
      ++g_coverage.checks;
      ++g_coverage.reasons[want.reason];
      g_coverage.replayed += want.replayed;
      if (const auto it = model.plans.find(c); it != model.plans.end()) {
        const ModelPlan& mp = it->second;
        g_coverage.monthly_checks += mp.monthly;
        g_coverage.boosted_checks += mp.boost > 0 && NowMs() < mp.boost_until_ms;
        g_coverage.topped_up_checks += mp.topup > 0 && mp.topup_period == PeriodOf(mp.monthly, NowMs(), mp.anchor).name;
        g_coverage.anchored_checks += mp.anchor != 1;
      }
    } else if (op < 87) {
      const std::int64_t ms = RandomStep(rng);
      g_coverage.rollovers += (NowMs() + ms) / kDayMs != NowMs() / kDayMs;
      g_coverage.month_rollovers += PeriodOf(true, NowMs() + ms).id != PeriodOf(true, NowMs()).id;
      for (const auto& [customer, mp] : model.plans) {
        g_coverage.anchored_rollovers +=
            mp.anchor != 1 && PeriodOf(true, NowMs() + ms, mp.anchor).id != PeriodOf(true, NowMs(), mp.anchor).id;
      }
      Advance(ms);
      what = "advance " + std::to_string(ms) + " ms";
    } else if (op < 98) {
      if (redis.down) continue;  // the admin tool cannot write while Redis is down
      std::optional<Fields> fields;
      ModelPlan plan;
      if (op >= 94 && redis.hashes.count(PlanKey(c)) && !model.plans[c].suspended) {
        // What the management API does to a plan it leaves otherwise alone (3.9.3): extend a boost
        // with the same units, end it, or add units to this period's top-up.
        fields = redis.hashes[PlanKey(c)];
        plan = model.plans[c];
        const int change = std::uniform_int_distribution<int>(0, 2)(rng);
        if (change == 0) {
          plan.boost = plan.boost ? plan.boost : 3;
          plan.boost_until_ms = NowMs() + std::uniform_int_distribution<std::int64_t>(1'000, 3'600'000)(rng);
        } else if (change == 1) {
          plan.boost_until_ms = NowMs();  // ended now; the units stay in the hash
        } else {
          const std::string current = PeriodOf(plan.monthly, NowMs(), plan.anchor).name;
          plan.topup = (plan.topup_period == current ? plan.topup : 0) + std::uniform_int_distribution<std::uint32_t>(1, 5)(rng);
          plan.topup_period = current;
          (*fields)["topup"] = std::to_string(plan.topup);
          (*fields)["topup_period"] = current;
        }
        if (change < 2) {
          (*fields)["boost"] = std::to_string(plan.boost);
          (*fields)["boost_until"] = std::to_string(plan.boost_until_ms);
        }
      } else {
        std::tie(fields, plan) = RandomPlan(rng);
      }
      // A customer's anchor is its subscription day: set once, kept by every plan change (store.py APPLY).
      if (const unsigned anchor = anchors.try_emplace(c, plan.anchor).first->second; fields && plan.anchor != anchor) {
        plan.anchor = anchor;
        if (anchor == 1) {
          fields->erase("anchor");
        } else {
          (*fields)["anchor"] = std::to_string(anchor);
        }
      }
      if (fields) {
        redis.hashes[PlanKey(c)] = *fields;
        model.plans[c] = plan;
      } else {
        redis.hashes.erase(PlanKey(c));
        model.plans.erase(c);
      }
      ++g_coverage.plan_changes;
      if (cfg.plan_cache) {  // the plan-change message, as PlanListener handles it
        plans.Invalidate(c);
        deny.Clear(c);
        model.Announce(c);
      }
      what = "plan " + c + (fields ? " daily " + std::to_string(plan.daily) + " burst " + std::to_string(plan.burst) +
                                         " policy " + std::to_string(static_cast<int>(plan.overage)) + " extra " +
                                         std::to_string(plan.extra) + (plan.suspended ? " suspended" : "") +
                                         (plan.monthly ? " monthly" : "") +
                                         (plan.boost ? " boost " + std::to_string(plan.boost) + " until " +
                                                           std::to_string(plan.boost_until_ms - NowMs()) + " ms"
                                                     : "") +
                                         (plan.topup ? " topup " + std::to_string(plan.topup) + " " + plan.topup_period : "")
                                   : " deleted");
    } else {
      redis.down = model.down = !redis.down;
      g_coverage.outages += redis.down;
      what = redis.down ? "redis down" : "redis back";
    }
    recent.push_back(what);
    if (recent.size() > 12) recent.pop_front();
  }
  for (const Alert& a : model.alerts) ++g_coverage.alerts[a.event];
  // Billing (P3): every month with billed overage is readable as the model counted it, the counter's
  // own month too; a finished month without billed overage is not filed.
  for (const std::string& c : customers) {
    const std::string counter = "daily_quota|" + c;
    const auto months = model.billed.count(c) ? model.billed.at(c) : decltype(model.billed)::mapped_type{};
    for (const auto& [month, totals] : months) {
      const auto got = redis.Billed(counter, month);
      const bool readable = month == redis.BillingMonth(counter) || totals.second > 0;
      const auto show = [](std::pair<std::int64_t, std::int64_t> t) {
        return "(" + std::to_string(t.first) + " in the plan, " + std::to_string(t.second) + " billed)";
      };
      if (readable && got != totals) {
        return c + " month " + std::to_string(month) + ": billing reads " + (got ? show(*got) : "nothing") +
               ", model " + show(totals);
      }
      if (!readable && got) return c + " month " + std::to_string(month) + " was filed without billed overage";
      g_coverage.billed_months += totals.second > 0;
      g_coverage.filed_months += readable && month != redis.BillingMonth(counter);
    }
    for (const std::int64_t month : redis.FiledMonths(counter)) {
      if (!months.count(month)) return c + " month " + std::to_string(month) + " was filed but admitted nothing";
    }
  }
  return "";
}

// Leased quota: N instances share the fake Redis; one customer on a plan of 100,000 a day or more.
std::string RunLeasedQuota(std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  g_now_ns = (20'000 * kDayMs + std::uniform_int_distribution<std::int64_t>(0, kDayMs - 1)(rng)) * 1'000'000;
  const int n = 1 + static_cast<int>(seed % 3);
  const std::uint32_t limit = std::vector<std::uint32_t>{100'000, 150'000, 250'000}[seed / 3 % 3];
  const std::uint32_t chunk = ChunkCap(Rule{.name = "daily_quota", .algorithm = Algorithm::kQuota, .limit = limit});
  const auto policy = static_cast<Overage>(std::uniform_int_distribution<int>(0, 2)(rng));
  const std::uint32_t extra = std::uniform_int_distribution<std::uint32_t>(0, limit / 20)(rng);
  static constexpr const char* kPolicy[] = {"hard", "grace", "cap"};

  ScriptRedis redis;
  redis.hashes[PlanKey("cust:big")] = {{"plan", "enterprise"}, {"daily", std::to_string(limit)},
                                       {"burst", "1000000"}, {"refill", "1000000"},
                                       {"overage", kPolicy[static_cast<int>(policy)]}, {"extra", std::to_string(extra)}};
  const RuleTable rules = CustomerRules();
  struct Instance {
    Instance(ScriptRedis& r, const RuleTable& rules)
        : plans(r, std::chrono::milliseconds(5000), &FakeNow),
          strict(r, plans, rules),
          leased(std::make_shared<LeasedCore>(r, plans, rules, 1024, &FakeNow)),
          core(leased, strict, plans, deny, rules, &FakeNow, {}, leased.get()) {}
    PlanStore plans;
    DenyCache deny;
    StrictCore strict;
    std::shared_ptr<LeasedCore> leased;
    CompositeCore core;
  };
  std::vector<std::unique_ptr<Instance>> instances;
  for (int i = 0; i < n; ++i) instances.push_back(std::make_unique<Instance>(redis, rules));

  std::map<std::int64_t, std::int64_t> admitted;  // units per UTC day, by the day of the answer
  std::int64_t total = 0;
  const auto describe = [&] {
    return "seed " + std::to_string(seed) + " (N=" + std::to_string(n) + " limit " + std::to_string(limit) + " C " +
           std::to_string(chunk) + " policy " + kPolicy[static_cast<int>(policy)] + " extra " + std::to_string(extra) + ")";
  };
  for (int step = 0; step < 6000; ++step) {
    const int op = std::uniform_int_distribution<int>(0, 999)(rng);
    if (op < 900) {
      const int roll = std::uniform_int_distribution<int>(0, 99)(rng);
      const std::uint32_t cost = roll < 80   ? std::uniform_int_distribution<std::uint32_t>(chunk / 2, chunk)(rng)
                                 : roll < 95 ? std::uniform_int_distribution<std::uint32_t>(chunk + 1, 2 * chunk)(rng)
                                             : 1;
      std::optional<CheckResult> got;
      instances[std::uniform_int_distribution<int>(0, n - 1)(rng)]->core.Check(CheckRequest{"cust:big", "customer", cost},
                                                                               [&got](const CheckResult& r) { got = r; });
      if (!got) return describe() + ": step " + std::to_string(step) + " got no answer";
      const bool ok = got->allowed ? (got->reason == Reason::kAllowed || got->reason == Reason::kOverage)
                                   : got->reason == Reason::kQuotaExhausted;
      if (!ok) return describe() + ": step " + std::to_string(step) + " answered " + Show(*got);
      if (got->allowed) {
        admitted[NowMs() / kDayMs] += cost;
        total += cost;
      }
    } else if (op < 999 || std::uniform_int_distribution<int>(0, 1)(rng) == 0) {  // a new day every ~2,000 steps
      Advance(std::uniform_int_distribution<int>(0, 99)(rng) < 90 ? std::uniform_int_distribution<std::int64_t>(0, 50)(rng)
                                                                  : std::uniform_int_distribution<std::int64_t>(1000, 30'000)(rng));
    } else {
      Advance(kDayMs - NowMs() % kDayMs + std::uniform_int_distribution<std::int64_t>(0, 3'600'000)(rng));
    }
  }
  // Redis never grants beyond the plan; what was admitted is what it granted, less what leases still hold.
  std::int64_t granted = 0;
  const auto days = redis.Granted("daily_quota|cust:big");
  for (const auto& [day, g] : days) {
    const auto [used, over] = g;
    if (used > limit + (policy == Overage::kGrace ? extra : 0) || over > (policy == Overage::kCap ? extra : 0)) {
      return describe() + ": day " + std::to_string(day) + " granted used " + std::to_string(used) + " overage " +
             std::to_string(over);
    }
    granted += used + over;
  }
  for (const auto& [day, units] : admitted) {
    const auto it = days.find(day);
    const std::int64_t same_day = it == days.end() ? 0 : it->second.first + it->second.second;
    if (units > same_day + std::int64_t{n} * chunk) {  // only units leased the day before may add to a day
      return describe() + ": day " + std::to_string(day) + " admitted " + std::to_string(units) + " of " +
             std::to_string(same_day) + " granted";
    }
  }
  g_leased.seeds += 1;
  g_leased.days += days.size();
  for (const auto& [day, g] : days) g_leased.days_at_limit += g.first >= limit;
  g_leased.admitted += total;
  g_leased.held += granted - total;
  if (total > granted || total < granted - std::int64_t{n} * chunk) {
    return describe() + ": admitted " + std::to_string(total) + ", granted " + std::to_string(granted) +
           ", leases can hold at most " + std::to_string(std::int64_t{n} * chunk);
  }
  return "";
}

// Merchant capacity (P3 gate): N instances serve one merchant on the shared fake Redis. Its capacity
// is `rate` calls a second (bucket `burst`), with `reserve`% only for plans with a fee, built from its
// terms by MerchantKeys as main.cc does. Free customers flood it at three times the capacity; paying
// customers ask for 70% of the reserve. Every window of the run admits at most
//   burst + rate x window + N x (C_shared + C_reserve)
// (C each bucket's chunk cap: what leases can hold beyond the buckets), Free calls alone at most the
// shared part's share of that, and paying customers keep almost all they asked for.
struct CapacityCoverage {
  std::uint64_t seeds = 0, admitted = 0, free_denied = 0, paid_asked = 0, paid_admitted = 0;
  double worst = 0;  // the largest share of its bound a window reached
} g_capacity;

std::string RunMerchantCapacity(std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  const auto pick = [&rng](std::uint32_t lo, std::uint32_t hi) { return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng); };
  g_now_ns = (20'000 * kDayMs + 3'600'000) * 1'000'000;
  ScriptRedis redis;
  const std::uint32_t rate = std::array{100u, 200u, 500u}[pick(0, 2)], burst = rate * pick(1, 2), reserve = 10 * pick(1, 3);
  const int n = static_cast<int>(pick(2, 4));
  redis.hashes[std::string(kMerchantKeysHash)] = {{Sha256Hex("rl_acme"), "acme"}};
  redis.hashes[std::string(kMerchantCapacityHash)] = {
      {"acme", "rate " + std::to_string(rate) + " burst " + std::to_string(burst) + " reserve " + std::to_string(reserve)}};
  MerchantKeys merchants(redis);
  if (!merchants.ReloadAndWait(std::chrono::seconds(1))) return "merchants not loaded";
  const MerchantLimits& limits = merchants.Resolve("rl_acme")->limits;
  const Rule& shared = *limits.capacity;
  const Rule& reserved = *limits.reserve;

  const std::vector<std::string> free{"acme|f1", "acme|f2", "acme|f3", "acme|f4", "acme|f5"}, paid{"acme|p1", "acme|p2"};
  for (const auto& c : free) redis.hashes[PlanKey(c)] = {{"plan", "free"}, {"daily", "90000"}, {"burst", "100000"}, {"refill", "100000"}};
  for (const auto& c : paid) redis.hashes[PlanKey(c)] = {{"plan", "pay"}, {"daily", "90000"}, {"burst", "100000"}, {"refill", "100000"}, {"fee", "1900"}};

  struct Instance {
    PlanStore plans;
    DenyCache deny;
    StrictCore strict;
    std::shared_ptr<LeasedCore> leased;
    CompositeCore core;
    Instance(ScriptRedis& r, const RuleTable& rules)
        : plans(r, std::chrono::milliseconds(5000), &FakeNow),
          strict(r, plans, rules),
          leased(std::make_shared<LeasedCore>(r, plans, rules, 1024, &FakeNow)),
          core(leased, strict, plans, deny, rules, &FakeNow, {}, leased.get()) {}
  };
  std::vector<std::unique_ptr<Instance>> instances;
  for (int i = 0; i < n; ++i) instances.push_back(std::make_unique<Instance>(redis, CustomerRules()));

  // One tick a millisecond for 4 s: Free calls at 3 x rate, paid at 0.7 x the reserve's rate.
  constexpr int kTicks = 4000;
  std::vector<std::int64_t> all(kTicks, 0), free_ok(kTicks, 0);
  std::int64_t paid_asked = 0, paid_admitted = 0;
  std::uniform_real_distribution<double> unit(0, 1);
  const double free_per_ms = 3.0 * rate / 1000, paid_per_ms = 0.7 * reserved.refill_per_sec / 1000;
  for (int t = 0; t < kTicks; ++t) {
    const int f = static_cast<int>(free_per_ms) + (unit(rng) < free_per_ms - static_cast<int>(free_per_ms));
    const int p = static_cast<int>(paid_per_ms) + (unit(rng) < paid_per_ms - static_cast<int>(paid_per_ms));
    for (int k = 0; k < f + p; ++k) {
      const bool is_paid = k >= f;
      const std::string& c = is_paid ? paid[pick(0, paid.size() - 1)] : free[pick(0, free.size() - 1)];
      CheckResult got;
      instances[pick(0, n - 1)]->core.Check(CheckRequest{c, "customer", 1, {}, &limits}, [&got](const CheckResult& r) { got = r; });
      if (got.reason != Reason::kAllowed && got.reason != Reason::kRateLimited) {
        return c + " at " + std::to_string(t) + " ms: unexpected answer " + Show(got);
      }
      all[t] += got.allowed;
      if (!is_paid) free_ok[t] += got.allowed;
      if (!is_paid) g_capacity.free_denied += !got.allowed;
      paid_asked += is_paid;
      paid_admitted += is_paid && got.allowed;
    }
    Advance(1);
  }

  const std::string terms = "rate " + std::to_string(rate) + " burst " + std::to_string(burst) + " reserve " +
                            std::to_string(reserve) + "%, " + std::to_string(n) + " instances";
  const std::int64_t c_shared = ChunkCap(shared), c_reserved = ChunkCap(reserved);
  for (const int w : {1, 10, 100, 1000, kTicks}) {
    std::int64_t in_all = 0, in_free = 0;
    for (int t = 0; t < kTicks; ++t) {
      in_all += all[t];
      in_free += free_ok[t];
      if (t >= w) {
        in_all -= all[t - w];
        in_free -= free_ok[t - w];
      }
      const double bound = burst + rate * w / 1000.0 + n * (c_shared + c_reserved);
      const double free_bound = shared.limit + shared.refill_per_sec * w / 1000.0 + n * c_shared;
      g_capacity.worst = std::max(g_capacity.worst, in_all / bound);
      if (in_all > bound) {
        return terms + ": " + std::to_string(in_all) + " admitted in " + std::to_string(w) + " ms ending at " +
               std::to_string(t) + ", bound " + std::to_string(bound);
      }
      if (in_free > free_bound) {
        return terms + ": Free took " + std::to_string(in_free) + " in " + std::to_string(w) + " ms, its share is " +
               std::to_string(free_bound) + ": the reserve leaked";
      }
    }
  }
  if (paid_admitted < paid_asked * 98 / 100) {  // asking 70% of the reserve, they lose at most 2%
    return terms + ": paying customers got " + std::to_string(paid_admitted) + " of " + std::to_string(paid_asked);
  }
  g_capacity.seeds += 1;
  g_capacity.paid_asked += paid_asked;
  g_capacity.paid_admitted += paid_admitted;
  for (const std::int64_t a : all) g_capacity.admitted += a;
  return "";
}

std::uint64_t EnvSeeds(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 10) : fallback;
}

void RunSeeds(const char* label, const std::function<std::string(std::uint64_t)>& run) {
  std::uint64_t first = 1, last = EnvSeeds("RL_PROPERTY_SEEDS", 300);
  if (const char* one = std::getenv("RL_PROPERTY_SEED")) first = last = std::strtoull(one, nullptr, 10);
  for (std::uint64_t seed = first; seed <= last; ++seed) {
    const std::string failure = run(seed);
    ASSERT_TRUE(failure.empty()) << label << " seed " << seed << ": " << failure << "\n  replay: RL_PROPERTY_SEED=" << seed;
  }
  std::printf("[property] %s: seeds %llu..%llu passed\n", label, static_cast<unsigned long long>(first),
              static_cast<unsigned long long>(last));
  if (g_coverage.checks > 0) {
    static constexpr const char* kName[] = {"ok", "rate_limited", "quota_exhausted", "unknown_rule", "unavailable",
                                            "unverified", "overage", "suspended"};
    std::printf("[property]   %llu checks (%llu replayed), %llu plan changes, %llu outages, %llu day rollovers; answers:",
                static_cast<unsigned long long>(g_coverage.checks), static_cast<unsigned long long>(g_coverage.replayed),
                static_cast<unsigned long long>(g_coverage.plan_changes), static_cast<unsigned long long>(g_coverage.outages),
                static_cast<unsigned long long>(g_coverage.rollovers));
    for (const auto& [reason, n] : g_coverage.reasons) {
      std::printf(" %s %llu", kName[static_cast<int>(reason)], static_cast<unsigned long long>(n));
    }
    std::printf("\n[property]   %llu month rollovers; checks on monthly plans %llu, with a boost %llu, with a top-up %llu; alerts:",
                static_cast<unsigned long long>(g_coverage.month_rollovers),
                static_cast<unsigned long long>(g_coverage.monthly_checks),
                static_cast<unsigned long long>(g_coverage.boosted_checks),
                static_cast<unsigned long long>(g_coverage.topped_up_checks));
    for (const auto& [event, n] : g_coverage.alerts) std::printf(" %s %llu", event.c_str(), static_cast<unsigned long long>(n));
    std::printf("\n[property]   billing: %llu customer months with billed overage, %llu of them filed by a new month\n",
                static_cast<unsigned long long>(g_coverage.billed_months),
                static_cast<unsigned long long>(g_coverage.filed_months));
    std::printf("[property]   anchored months (P5): %llu checks, %llu of a customer's periods ended at its anchor day\n",
                static_cast<unsigned long long>(g_coverage.anchored_checks),
                static_cast<unsigned long long>(g_coverage.anchored_rollovers));
    g_coverage = {};
  }
  if (g_leased.seeds > 0) {
    std::printf("[property]   %llu days, %llu of them at the plan's limit; %llu units admitted, %llu granted but still in leases at the end\n",
                static_cast<unsigned long long>(g_leased.days), static_cast<unsigned long long>(g_leased.days_at_limit),
                static_cast<unsigned long long>(g_leased.admitted), static_cast<unsigned long long>(g_leased.held));
    g_leased = {};
  }
  if (g_capacity.seeds > 0) {
    std::printf("[property]   %llu calls admitted; the fullest window reached %.0f%% of its bound; Free denied %llu; "
                "paying customers kept %llu of %llu calls asked\n",
                static_cast<unsigned long long>(g_capacity.admitted), 100 * g_capacity.worst,
                static_cast<unsigned long long>(g_capacity.free_denied),
                static_cast<unsigned long long>(g_capacity.paid_admitted),
                static_cast<unsigned long long>(g_capacity.paid_asked));
    g_capacity = {};
  }
}

TEST(Property, ExactPlansMatchTheModelWithPlanCacheAndMessages) {
  RunSeeds("plan cache + messages, strict engine", [](std::uint64_t s) { return RunExact(s, {true, false}); });
}

TEST(Property, ExactPlansMatchTheModelWithoutPlanCache) {
  RunSeeds("no plan cache, strict engine", [](std::uint64_t s) { return RunExact(s, {false, false}); });
}

TEST(Property, ExactPlansMatchTheModelOnTheLeasedEngine) {
  RunSeeds("plan cache + messages, leased engine", [](std::uint64_t s) { return RunExact(s, {true, true}); });
  RunSeeds("no plan cache, leased engine", [](std::uint64_t s) { return RunExact(s, {false, true}); });
}

TEST(Property, LeasedQuotaGrantsWithinThePlanAndAdmitsWhatWasGranted) {
  RunSeeds("leased quota bounds", RunLeasedQuota);
}

TEST(Property, MerchantCapacityHoldsAndPayingCustomersKeepTheReserve) {
  RunSeeds("merchant capacity under a Free flood", RunMerchantCapacity);
}

}  // namespace
}  // namespace ratelimiter
