// Microbenchmark of the engine interface call overhead. Real lease-path benchmarks arrive in Week 3.
#include <benchmark/benchmark.h>

#include "core/allow_all_core.h"

static void BM_AllowAllCheck(benchmark::State& state) {
  static ratelimiter::AllowAllCore core;
  for (auto _ : state) {
    core.Check({"user:1", "api_default", 1},
               [](const ratelimiter::CheckResult& r) {
                 bool allowed = r.allowed;
                 benchmark::DoNotOptimize(allowed);
               });
  }
}
BENCHMARK(BM_AllowAllCheck)->ThreadRange(1, 8);
