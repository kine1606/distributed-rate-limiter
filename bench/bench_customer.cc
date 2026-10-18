// What the shared structures of the customer path cost per check (Week 4, lock-free question):
// a plan cache hit, a deny cache lookup, and a usage event. Arg(1) is one hot customer (every
// thread on one shard), Arg(10000) spreads the customers. Run from the repo root:
//   build/release/bench/bench_customer --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
// Throughput is items_per_second summed over threads (real time).
#include <benchmark/benchmark.h>

#include <memory>
#include <string>
#include <vector>

#include "lease/deny_cache.h"
#include "plan/plan_store.h"
#include "usage/usage_log.h"

namespace ratelimiter {
namespace {

class InlineRedis final : public RedisBackend {
 public:
  using RedisBackend::Eval;
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done({.allowed = true});
  }
  void HGetAll(std::string, HashCallback done) override { done({.fields = {{"daily", "1000000"}}}); }
};

std::vector<std::string> Customers(std::int64_t n) {
  std::vector<std::string> out;
  for (std::int64_t i = 0; i < n; ++i) out.push_back("cust:ent:" + std::to_string(i));
  return out;
}

void BM_PlanHit(benchmark::State& s) {
  static InlineRedis redis;
  static std::unique_ptr<PlanStore> plans;
  static std::vector<std::string> customers;
  if (s.thread_index() == 0) {
    plans = std::make_unique<PlanStore>(redis, std::chrono::hours(1));
    customers = Customers(s.range(0));
    for (const auto& c : customers) plans->Get(c, [](const Plan*) {});  // warm: every Get below hits
  }
  std::size_t i = s.thread_index();
  std::uint32_t sum = 0;
  for (auto _ : s) {
    plans->Get(customers[i++ % customers.size()], [&sum](const Plan* p) { sum += p->daily; });
  }
  benchmark::DoNotOptimize(sum);
  s.SetItemsProcessed(s.iterations());
}

// Arg(0): nobody is denied, the common case. Arg(1): the hot customer is denied (a flood).
void BM_DenyLookup(benchmark::State& s) {
  static DenyCache deny;
  if (s.thread_index() == 0 && s.range(0) == 1) deny.Deny("cust:ent:0", 1, 0, 60'000);
  std::uint64_t sum = 0;
  for (auto _ : s) sum += deny.DeniedFor("cust:ent:0", 1, 1);
  benchmark::DoNotOptimize(sum);
  s.SetItemsProcessed(s.iterations());
}

void BM_UsageRecord(benchmark::State& s) {
  static std::unique_ptr<UsageLog> log;
  if (s.thread_index() == 0) log = std::make_unique<UsageLog>("/dev/null", "bench", std::size_t{1} << 20);
  std::int64_t kept = 0;  // a dropped event is cheaper than a kept one: count only what was kept
  for (auto _ : s) kept += log->Record("cust:ent:0", Reason::kAllowed, 1);
  s.SetItemsProcessed(kept);
  if (s.thread_index() == 0) s.counters["dropped"] = static_cast<double>(log->Dropped());
}

BENCHMARK(BM_PlanHit)->Arg(1)->Arg(10'000)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(BM_DenyLookup)->Arg(0)->Arg(1)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(BM_UsageRecord)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();

}  // namespace
}  // namespace ratelimiter
