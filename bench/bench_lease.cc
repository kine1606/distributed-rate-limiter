// Lease hot-path microbenchmarks (ROADMAP Week 3 ablation): what a hit costs, what contention and
// false sharing cost, and whether a hit allocates. Run from the repo root:
//   build/release/bench/bench_lease --benchmark_repetitions=5 --benchmark_report_aggregates_only=true
// Throughput is items_per_second summed over threads (real time); allocs/op counts every operator
// new during the timed loop, across all threads.
#include <benchmark/benchmark.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "core/leased_core.h"
#include "lease/lease_table.h"

namespace {

std::atomic<std::uint64_t> g_allocs{0};

}  // namespace

// Counts heap allocations in this binary; the benchmarks report the count per operation.
// GCC flags free() on memory from this operator new as a mismatch; for a replaced pair it is not.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
void* operator new(std::size_t n) {
  g_allocs.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(n == 0 ? 1 : n)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#pragma GCC diagnostic pop

namespace ratelimiter {
namespace {

constexpr std::int64_t kForever = std::int64_t{1} << 60;  // tokens that never run out in a benchmark

void ReportAllocs(benchmark::State& s, std::uint64_t before) {
  s.SetItemsProcessed(s.iterations());
  if (s.thread_index() == 0) {
    s.counters["allocs/op"] = benchmark::Counter(static_cast<double>(g_allocs.load() - before),
                                                 benchmark::Counter::kAvgIterations);
  }
}

// --- Contention on one lease: every thread takes from the same cache line ---------------------

Lease& SharedLease() {
  static const Rule rule{.name = "b", .limit = 1000, .refill_per_sec = 1};
  static Lease lease(rule, "hot");
  return lease;
}

void BM_TakeCas_OneLease(benchmark::State& s) {  // the engine's TryTake: CAS loop, never below zero
  Lease& l = SharedLease();
  if (s.thread_index() == 0) l.tokens = kForever;
  const std::uint64_t before = g_allocs.load();
  for (auto _ : s) {
    std::int64_t left = TryTake(l, 1);
    benchmark::DoNotOptimize(left);
  }
  ReportAllocs(s, before);
}

void BM_TakeFetchSub_OneLease(benchmark::State& s) {  // the alternative: subtract, undo on overdraw
  Lease& l = SharedLease();
  if (s.thread_index() == 0) l.tokens = kForever;
  const std::uint64_t before = g_allocs.load();
  for (auto _ : s) {
    std::int64_t left = l.tokens.fetch_sub(1, std::memory_order_relaxed) - 1;
    if (left < 0) l.tokens.fetch_add(1, std::memory_order_relaxed);
    benchmark::DoNotOptimize(left);
  }
  ReportAllocs(s, before);
}

// --- False sharing: every thread takes from its own counter, packed or one per cache line ------

struct Packed {
  std::atomic<std::int64_t> tokens{kForever};
};
struct alignas(64) Padded {
  std::atomic<std::int64_t> tokens{kForever};
};

template <typename Slot>
void BM_OwnCounter(benchmark::State& s) {
  static Slot slots[64];
  std::atomic<std::int64_t>& mine = slots[s.thread_index() % 64].tokens;
  for (auto _ : s) {
    std::int64_t t = mine.load(std::memory_order_relaxed);
    while (!mine.compare_exchange_weak(t, t - 1, std::memory_order_relaxed)) {
    }
    benchmark::DoNotOptimize(t);
  }
  s.SetItemsProcessed(s.iterations());
}

// --- The real lookup: LeaseTable::FindOrCreate + TryTake, one hot key or 10,000 keys ------------

const std::vector<std::string>& Keys() {
  static const std::vector<std::string> keys = [] {
    std::vector<std::string> k;
    for (int i = 0; i < 10'000; ++i) k.push_back("cust:" + std::to_string(i));
    return k;
  }();
  return keys;
}

void BM_FindAndTake(benchmark::State& s) {
  static const Rule rule{.name = "api_default", .limit = 1000, .refill_per_sec = 1};
  static LeaseTable table(std::size_t{1} << 20);
  static const bool filled = [] {
    for (const auto& k : Keys()) table.FindOrCreate(rule, k)->tokens = kForever;
    return true;
  }();
  bool ready = filled;
  benchmark::DoNotOptimize(ready);
  const auto n = static_cast<std::size_t>(s.range(0));
  std::size_t i = static_cast<std::size_t>(s.thread_index()) * 997 % n;
  const std::uint64_t before = g_allocs.load();
  for (auto _ : s) {
    std::int64_t left = TryTake(*table.FindOrCreate(rule, Keys()[i]), 1);
    benchmark::DoNotOptimize(left);
    if (++i == n) i = 0;
  }
  ReportAllocs(s, before);
}

// --- The whole engine on a hit: rule lookup, lease lookup, take, callback -----------------------

class GrantAll final : public RedisBackend {  // Redis that grants every refill at once
 public:
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    ScriptResult r;
    r.allowed = true;
    done(r);
  }
  void HGetAll(std::string, HashCallback done) override { done({}); }
};

struct Engine {
  GrantAll redis;
  PlanStore plans{redis};
  // A huge limit: one refill (10% = 400M tokens) per key lasts the whole run.
  LeasedCore core{redis, plans, {{"b", Rule{.name = "b", .limit = 4'000'000'000u, .refill_per_sec = 1e9}}}};
  Engine() {
    for (const auto& k : Keys()) core.Check({k, "b", 1}, [](const CheckResult&) {});
  }
};

void BM_LeasedCheckHit(benchmark::State& s) {
  static Engine engine;
  const auto n = static_cast<std::size_t>(s.range(0));
  std::size_t i = static_cast<std::size_t>(s.thread_index()) * 997 % n;
  const std::uint64_t before = g_allocs.load();
  for (auto _ : s) {
    engine.core.Check({Keys()[i], "b", 1}, [](const CheckResult& r) {
      bool allowed = r.allowed;
      benchmark::DoNotOptimize(allowed);
    });
    if (++i == n) i = 0;
  }
  ReportAllocs(s, before);
}

BENCHMARK(BM_TakeCas_OneLease)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(BM_TakeFetchSub_OneLease)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK_TEMPLATE(BM_OwnCounter, Packed)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK_TEMPLATE(BM_OwnCounter, Padded)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(BM_FindAndTake)->Arg(1)->Arg(10'000)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();
BENCHMARK(BM_LeasedCheckHit)->Arg(1)->Arg(10'000)->Threads(1)->Threads(4)->Threads(16)->UseRealTime();

}  // namespace
}  // namespace ratelimiter
