// What ResilientBackend adds to each Redis call (src/reliability/), measured on a fake Redis that
// answers inline, so only the layer itself is timed: the breaker check, the answer wrapper and, with a
// deadline, registering the attempt. One quota script per iteration (the exact path's call).
//   build/release/bench/bench_resilient
#include <benchmark/benchmark.h>

#include <chrono>
#include <memory>

#include "reliability/resilient_backend.h"

namespace {

using namespace ratelimiter;  // NOLINT

class InlineRedis final : public RedisBackend {
 public:
  using RedisBackend::Eval;
  void Eval(const Rule&, std::string_view, std::uint32_t, std::string_view, ScriptCallback done) override {
    done({.allowed = true, .remaining = 1});
  }
  void HGetAll(std::string, HashCallback done) override { done({}); }
};

const Rule kQuota{.name = "daily_quota", .algorithm = Algorithm::kQuota, .limit = 1'000'000};

void Run(benchmark::State& s, std::chrono::milliseconds deadline) {
  static InlineRedis redis;
  static SlotBreakers breakers;
  static std::unique_ptr<ResilientBackend> backend;
  if (s.thread_index() == 0) backend = std::make_unique<ResilientBackend>(redis, breakers, RetryOptions{.deadline = deadline});
  const std::string key = "cust:" + std::to_string(s.thread_index());  // its own slot group, mostly
  std::uint64_t answered = 0;
  for (auto _ : s) backend->Eval(kQuota, key, 1, [&answered](const ScriptResult& r) { answered += r.allowed; });
  benchmark::DoNotOptimize(answered);
  s.SetItemsProcessed(s.iterations());
  if (s.thread_index() == 0) {
    s.counters["deadline_misses"] = static_cast<double>(backend->DeadlinesMissed());
  }
}

void BM_NoDeadline(benchmark::State& s) { Run(s, std::chrono::milliseconds(0)); }
void BM_Deadline50ms(benchmark::State& s) { Run(s, std::chrono::milliseconds(50)); }

BENCHMARK(BM_NoDeadline)->Threads(1)->Threads(4)->Threads(12)->UseRealTime();
BENCHMARK(BM_Deadline50ms)->Threads(1)->Threads(4)->Threads(12)->UseRealTime();

}  // namespace
