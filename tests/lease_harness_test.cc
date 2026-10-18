// Deterministic harness for the lease protocol (ROADMAP Week 3). N LeasedCore instances share one
// fake Redis that runs token_bucket.lua's arithmetic at the moment it answers, and answers pending
// calls in a seeded random order (sometimes with an error) under a fake clock. Each seed must keep
// the burst bound for every key and every time window. A failing seed prints itself; replay it with
// RL_HARNESS_SEED=<seed>. RL_HARNESS_SEEDS sets how many seeds run (default 2,000).
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include "core/leased_core.h"

namespace ratelimiter {
namespace {

std::int64_t g_now_ns = 0;  // the fake clock; the harness is single-threaded
std::int64_t FakeNow() { return g_now_ns; }
std::int64_t NowMs() { return g_now_ns / 1'000'000; }

// A Redis that holds one token bucket per key and runs token_bucket.lua's math (milli-tokens, a ms
// clock, refill on use) when a call is answered, which is when the script "runs".
class FakeRedis final : public RedisBackend {
 public:
  struct Call {
    std::string key;
    std::uint32_t capacity;
    double rate;
    std::uint32_t requested;
    ScriptCallback done;
  };

  void Eval(const Rule& rule, std::string_view key, std::uint32_t requested, std::string_view, ScriptCallback done) override {
    pending.push_back({std::string(key), rule.limit, rule.refill_per_sec, requested, std::move(done)});
  }
  void HGetAll(std::string, HashCallback done) override { done({}); }

  // Answers one pending call chosen by `rng`; with probability `p_error` it fails instead.
  void AnswerOne(std::mt19937_64& rng, double p_error) {
    const std::size_t i = std::uniform_int_distribution<std::size_t>(0, pending.size() - 1)(rng);
    Call c = std::move(pending[i]);
    pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
    ScriptResult r;
    if (std::bernoulli_distribution(p_error)(rng)) {
      r.redis_error = true;
    } else {
      r = Run(c);
    }
    c.done(r);  // may call Eval again: `c` is already out of `pending`
  }

  std::vector<Call> pending;
  std::map<std::string, std::uint64_t> granted;  // tokens Redis handed out, per key

 private:
  struct Bucket {
    double tokens;  // milli-tokens
    std::int64_t last_ms;
  };

  ScriptResult Run(const Call& c) {
    const double cap = c.capacity * 1000.0;
    auto [it, fresh] = buckets_.try_emplace(c.key, Bucket{cap, NowMs()});
    Bucket& b = it->second;
    b.tokens = std::min(cap, b.tokens + static_cast<double>(std::max<std::int64_t>(0, NowMs() - b.last_ms)) * c.rate);
    b.last_ms = NowMs();
    ScriptResult r;
    const double need = c.requested * 1000.0;
    if (need <= b.tokens) {
      b.tokens -= need;
      r.allowed = true;
      granted[c.key] += c.requested;
    } else if (need <= cap && c.rate > 0) {
      r.retry_after_ms = static_cast<std::uint32_t>(std::ceil((need - b.tokens) / c.rate));
    }
    r.remaining = static_cast<std::uint32_t>(b.tokens / 1000.0);
    return r;
  }

  std::map<std::string, Bucket> buckets_;
};

struct Admission {
  std::int64_t ms;
  std::uint32_t cost;
};

struct Outcome {
  std::string failure;  // empty: every invariant held
  double worst_excess_over_nc = 0;  // max over windows of (admitted - exact bucket) / (N x C)
  std::uint64_t admitted = 0, checks = 0, refills = 0, chunk_changes = 0;
  std::string trace;  // admissions in order, to prove a replay is identical
};

Outcome RunSeed(std::uint64_t seed) {
  std::mt19937_64 rng(seed);
  const auto uniform = [&rng](std::int64_t lo, std::int64_t hi) {
    return std::uniform_int_distribution<std::int64_t>(lo, hi)(rng);
  };
  const auto chance = [&rng](double p) { return std::bernoulli_distribution(p)(rng); };

  g_now_ns = 1'000'000'000;
  const int instances = static_cast<int>(uniform(1, 4));
  const auto limit = static_cast<std::uint32_t>(uniform(10, 2000));
  const Rule rule{.name = "b", .limit = limit, .refill_per_sec = static_cast<double>(uniform(1, limit)),
                  .failure_policy = FailurePolicy::kFailClosed};  // fail open would admit without Redis
  const std::vector<std::string> keys{"k0", "k1", "k2"};
  const auto key_count = static_cast<std::size_t>(uniform(1, 3));

  Outcome out;
  FakeRedis redis;
  PlanStore plans(redis);
  std::vector<std::unique_ptr<LeasedCore>> cores;
  for (int i = 0; i < instances; ++i) {
    cores.push_back(std::make_unique<LeasedCore>(
        redis, plans, RuleTable{{"b", rule}}, 1024, &FakeNow, [&](const Lease&, std::uint32_t, std::uint32_t to) {
          ++out.chunk_changes;
          if (to < ChunkMin(rule) || to > ChunkCap(rule)) out.failure = "chunk " + std::to_string(to) + " out of range";
        }));
  }

  std::map<std::string, std::vector<Admission>> admitted;
  std::ostringstream trace;
  const auto check = [&] {
    const std::string& key = keys[static_cast<std::size_t>(uniform(0, static_cast<std::int64_t>(key_count) - 1))];
    const auto cost = static_cast<std::uint32_t>(chance(0.9) ? 1 : uniform(2, std::min<std::int64_t>(5, limit)));
    ++out.checks;
    cores[static_cast<std::size_t>(uniform(0, instances - 1))]->Check(
        {key, "b", cost}, [&admitted, &trace, &key, cost](const CheckResult& r) {
          if (!r.allowed) return;
          admitted[key].push_back({NowMs(), cost});
          trace << key << '@' << NowMs() << '+' << cost << ' ';
        });
  };

  for (int step = 0; step < 3000; ++step) {
    const double u = std::uniform_real_distribution<double>(0, 1)(rng);
    if (u < 0.55) {
      check();
    } else if (u < 0.9) {
      if (!redis.pending.empty()) {
        ++out.refills;
        redis.AnswerOne(rng, 0.02);
      }
    } else {
      g_now_ns += chance(0.9) ? uniform(0, 5'000'000) : uniform(0, 3'000'000'000);  // ms steps, rare long gaps
    }
  }
  for (int i = 0; i < 100'000 && !redis.pending.empty(); ++i) redis.AnswerOne(rng, 0.0);  // land what is out
  if (!redis.pending.empty()) out.failure = "refills never settled";

  // Every key, every window [t_i, t_j] of admissions: admitted <= limit + rate x (t_j - t_i) + N x C.
  // With prefix sums P, that is (P[j+1] - r t_j) - min over i <= j of (P[i] - r t_i) <= limit + N x C,
  // one pass per key.
  const double rate_per_ms = rule.refill_per_sec / 1000.0;
  const double nc = static_cast<double>(instances) * ChunkCap(rule);
  for (const auto& [key, list] : admitted) {
    std::uint64_t total = 0;
    double min_start = 0, worst = -1e18;
    for (std::size_t j = 0; j < list.size(); ++j) {
      const double start = static_cast<double>(total) - rate_per_ms * static_cast<double>(list[j].ms);
      min_start = j == 0 ? start : std::min(min_start, start);
      total += list[j].cost;
      worst = std::max(worst, static_cast<double>(total) - rate_per_ms * static_cast<double>(list[j].ms) - min_start);
    }
    out.admitted += total;
    out.worst_excess_over_nc = std::max(out.worst_excess_over_nc, (worst - limit) / nc);
    if (worst > limit + nc + 1e-6) {
      std::ostringstream why;
      why << key << ": a window admitted " << worst - limit << " over the exact bucket, more than N x C = " << nc
          << " (N=" << instances << " limit=" << limit << " rate=" << rule.refill_per_sec << "/s C=" << ChunkCap(rule)
          << ")";
      out.failure = why.str();
    }
    if (total > redis.granted[key]) out.failure = key + ": admitted more tokens than Redis granted";
  }
  out.trace = trace.str();
  return out;
}

std::uint64_t EnvOr(const char* name, std::uint64_t fallback) {
  const char* v = std::getenv(name);
  return v ? std::strtoull(v, nullptr, 10) : fallback;
}

TEST(LeaseHarness, EverySeedKeepsTheBurstBound) {
  const std::uint64_t only = EnvOr("RL_HARNESS_SEED", 0);
  const std::uint64_t first = only ? only : 1, last = only ? only : EnvOr("RL_HARNESS_SEEDS", 2000);
  double worst = 0;
  std::uint64_t admitted = 0, checks = 0, changes = 0;
  for (std::uint64_t seed = first; seed <= last; ++seed) {
    const Outcome o = RunSeed(seed);
    ASSERT_TRUE(o.failure.empty()) << "seed " << seed << ": " << o.failure << " (replay: RL_HARNESS_SEED=" << seed
                                   << ")";
    worst = std::max(worst, o.worst_excess_over_nc);
    admitted += o.admitted;
    checks += o.checks;
    changes += o.chunk_changes;
  }
  std::printf("[harness] seeds %llu..%llu: %llu checks, %llu tokens admitted, %llu chunk changes, worst window "
              "over-admission %.2f x (N x C)\n",
              static_cast<unsigned long long>(first), static_cast<unsigned long long>(last),
              static_cast<unsigned long long>(checks), static_cast<unsigned long long>(admitted),
              static_cast<unsigned long long>(changes), worst);
  EXPECT_GT(admitted, 0u);  // the runs did exercise the lease
  EXPECT_GT(changes, 0u);   // and the adaptive chunk
}

TEST(LeaseHarness, ASeedReplaysExactly) {
  const Outcome a = RunSeed(4242), b = RunSeed(4242);
  EXPECT_FALSE(a.trace.empty());
  EXPECT_EQ(a.trace, b.trace);
  EXPECT_EQ(a.refills, b.refills);
}

}  // namespace
}  // namespace ratelimiter
