// Property-based tests (Week 6, docs/THESIS_PLAN.md). Random histories of customer checks, retries,
// plan changes, Redis outages, clock jumps and UTC day rollovers run through the real engine
// (CompositeCore over StrictCore or LeasedCore, PlanStore, DenyCache) on a fake Redis that executes
// the scripts' rules (lua/token_bucket.lua, quota.lua, return_lease.lua) under a fake clock.
//
// 1. Exact plans: every answer and every usage event must equal a reference model that states the
//    product rules of Chapter 3, 3.4 in one sequential function, written from those rules.
// 2. Leased quota (plans of 100,000 a day or more) admits differently from the exact path by design,
//    so it is checked against its bounds instead (3.4.5): Redis never grants beyond the plan, and
//    what N instances admitted is what Redis granted minus at most N x C units still held in leases.
//
// The scripts' arithmetic itself is tested against a real Redis (lua_scripts_test.cc). A failing seed
// prints itself; replay it with RL_PROPERTY_SEED=<seed>. RL_PROPERTY_SEEDS sets how many seeds run
// per configuration (default 300).
#include <gtest/gtest.h>

#include <algorithm>
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

namespace ratelimiter {
namespace {

std::int64_t g_now_ns = 0;
std::int64_t FakeNow() { return g_now_ns; }
std::int64_t NowMs() { return g_now_ns / 1'000'000; }
void Advance(std::int64_t ms) { g_now_ns += ms * 1'000'000; }
constexpr std::int64_t kDayMs = 86'400'000;
constexpr std::int64_t kIdTtlMs = 300'000;

using Fields = std::unordered_map<std::string, std::string>;

// ---- A fake Redis that runs the scripts' rules (transcribed from lua/) on the fake clock ----

class ScriptRedis final : public RedisBackend {
 public:
  bool down = false;                   // every call fails, as with a dead master
  std::map<std::string, Fields> hashes;  // plan hashes, by key

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

  // Units the quota counter `rule|key` granted per UTC day: {day: (used, overage_used)}.
  std::map<std::int64_t, std::pair<std::int64_t, std::int64_t>> Granted(const std::string& counter) const {
    auto out = history_.count(counter) ? history_.at(counter) : decltype(history_)::mapped_type{};
    if (const auto it = counters_.find(counter); it != counters_.end()) out[it->second.day] = {it->second.used, it->second.over};
    return out;
  }

 private:
  struct BucketState {
    double tokens_m;
    std::int64_t last_ms, expires_ms;
  };
  struct CounterState {
    std::int64_t day, used, over;
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
    const std::int64_t now = NowMs(), day = now / kDayMs, limit = rule.limit, cost = requested,
                       extra = rule.overage_extra;
    ScriptResult r;
    r.reset_after_ms = static_cast<std::uint32_t>((day + 1) * kDayMs - now);
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
    CounterState& c = counters_.try_emplace(counter, CounterState{day, 0, 0}).first->second;
    if (c.day != day) {
      history_[counter][c.day] = {c.used, c.over};
      c = {day, 0, 0};
    }
    bool allowed = false, overage = false;
    if (c.used + cost <= limit) {
      c.used += cost;
      allowed = true;
    } else if (rule.overage == Overage::kGrace) {
      if (c.used + cost <= limit + extra) {
        c.used += cost;
        allowed = true;
      }
    } else if (rule.overage == Overage::kCap) {
      const std::int64_t in_plan = std::max<std::int64_t>(0, std::min(cost, limit - c.used));
      if (c.over + cost - in_plan <= extra) {
        c.used += in_plan;
        c.over += cost - in_plan;
        allowed = overage = true;
      }
    }
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
  struct Day {
    std::int64_t day = -1, used = 0, over = 0;
  };
  struct Answer {
    bool allowed;
    std::int64_t remaining;
    bool overage;
    std::int64_t expires_ms;
  };
  struct Denial {
    std::int64_t until_ms;
    std::tuple<std::uint32_t, std::uint32_t, Overage> terms;  // a denial holds only under its plan's quota terms
  };

  static std::tuple<std::uint32_t, std::uint32_t, Overage> Terms(const ModelPlan& p) { return {p.daily, p.extra, p.overage}; }

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
    // 4. The daily quota of the current UTC day under the plan's overage policy; a request id
    // decided in the last five minutes gets its first answer again and is not counted.
    const std::int64_t day = now / kDayMs;
    CheckResult r;
    r.limit = plan.daily;
    r.reset_after_ms = static_cast<std::uint32_t>((day + 1) * kDayMs - now);
    Answer a{};
    if (const auto it = answers_.find(c + "|" + id); !id.empty() && it != answers_.end() && now <= it->second.expires_ms) {
      a = it->second;
      r.replayed = true;
    } else {
      Day& d = days_[c];
      if (d.day != day) d = {day, 0, 0};
      const std::int64_t room = static_cast<std::int64_t>(plan.daily) - d.used;
      if (cost <= room) {  // within the plan, whatever the policy
        d.used += cost;
        a.allowed = true;
      } else if (plan.overage == Overage::kGrace && cost <= room + plan.extra) {  // free extra units
        d.used += cost;
        a.allowed = true;
      } else if (plan.overage == Overage::kCap) {  // billed beyond the plan, up to the cap
        const std::int64_t inside = std::clamp<std::int64_t>(room, 0, cost);
        if (d.over + (cost - inside) <= plan.extra) {
          d.used += inside;
          d.over += cost - inside;
          a.allowed = a.overage = true;
        }
      }
      a.remaining = std::max<std::int64_t>(0, static_cast<std::int64_t>(plan.daily) - d.used);
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
  std::map<std::string, Day> days_;
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
  return {Fields{{"plan", "pay"},
                 {"daily", std::to_string(m.daily)},
                 {"burst", std::to_string(m.burst)},
                 {"refill", std::to_string(m.refill)},
                 {"overage", kPolicy[static_cast<int>(m.overage)]},
                 {"extra", std::to_string(m.extra)},
                 {"failure", m.fail_open ? "open" : "closed"}},
          m};
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
  if (kind < 99) return std::max<std::int64_t>(0, to_midnight + ms(-1500, 1500));  // around the reset
  return to_midnight + ms(0, 3 * 3'600'000);  // into the next UTC day
}

// What the histories reached, summed over every seed of a run: a pass only means something if every
// branch was taken.
struct Coverage {
  std::map<Reason, std::uint64_t> reasons;
  std::uint64_t checks = 0, replayed = 0, plan_changes = 0, outages = 0, rollovers = 0;
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
      if (!Same(*got, want) || usage != model.usage) {
        std::string trail;
        for (const auto& s : recent) trail += "\n    " + s;
        return "step " + std::to_string(step) + " (" + what + "): got " + Show(*got) + ", model " + Show(want) +
               (usage != model.usage ? " (usage events differ)" : "") + "\n  last steps:" + trail;
      }
      what += " -> " + Show(want);
      ++g_coverage.checks;
      ++g_coverage.reasons[want.reason];
      g_coverage.replayed += want.replayed;
    } else if (op < 87) {
      const std::int64_t ms = RandomStep(rng);
      g_coverage.rollovers += (NowMs() + ms) / kDayMs != NowMs() / kDayMs;
      Advance(ms);
      what = "advance " + std::to_string(ms) + " ms";
    } else if (op < 98) {
      if (redis.down) continue;  // the admin tool cannot write while Redis is down
      auto [fields, plan] = RandomPlan(rng);
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
                                         std::to_string(plan.extra) + (plan.suspended ? " suspended" : "")
                                   : " deleted");
    } else {
      redis.down = model.down = !redis.down;
      g_coverage.outages += redis.down;
      what = redis.down ? "redis down" : "redis back";
    }
    recent.push_back(what);
    if (recent.size() > 12) recent.pop_front();
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
    std::printf("\n");
    g_coverage = {};
  }
  if (g_leased.seeds > 0) {
    std::printf("[property]   %llu days, %llu of them at the plan's limit; %llu units admitted, %llu granted but still in leases at the end\n",
                static_cast<unsigned long long>(g_leased.days), static_cast<unsigned long long>(g_leased.days_at_limit),
                static_cast<unsigned long long>(g_leased.admitted), static_cast<unsigned long long>(g_leased.held));
    g_leased = {};
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

}  // namespace
}  // namespace ratelimiter
