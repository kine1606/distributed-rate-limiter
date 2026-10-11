# Benchmarks

> Week 2 (baseline), Week 3 (microbenchmarks), Weeks 7 and 8 (full matrix) · Status: one placeholder microbenchmark

## What this module does

Benchmarks turn "it is fast" into numbers you can defend. There are two kinds, and the thesis needs
both. Microbenchmarks (Google Benchmark) time one function in a loop, such as `TryTake`, with no
network. Load tests send real gRPC traffic at a running server and measure throughput and latency
percentiles end to end. Microbenchmarks explain why; load tests show what.

## Rules for every run

These rules make your numbers trustworthy, which matters more than making them big.

- **Record the setup next to the result:** CPU model, cores, RAM, OS and WSL version, instance
  count, Redis masters, rule config, chunk size, key distribution, and the exact command.
- **Warm up, then measure.** Discard the first 10 seconds; measure for at least 30.
- **Repeat 5 times and report the median,** plus min and max so the reader sees the noise.
- **Plug in the laptop and use the high-performance power mode;** close the browser and IDE.
- **Never present an extrapolation as a measurement** (CLAUDE.md rule).

## Week 2: baseline

```bash
# 1 instance, StrictCore, Free customers spread over 10,000 keys
ghz --insecure --proto proto/rate_limiter.proto \
    --call ratelimiter.v1.RateLimiter.Check \
    -d '{"key":"cust:{{randomInt 1 10000}}","rule":"daily_quota","cost":1}' \
    -c 64 -z 30s --connections 4 \
    -O json -o results/2026-10-xx_strict_1inst_c64.json \
    127.0.0.1:50051
```

Hint: increase `-c` (concurrent workers) until throughput stops growing; that plateau is the
number to report, together with the latency at that point. Report p50, p99 and p999 from the ghz
JSON. Run once with `AllowAllCore` as well: the difference between AllowAll and Strict is the cost
of Redis, which is exactly what the lease removes.

## Week 3: microbenchmarks

```cpp
// bench_lease.cc
static void BM_TryTake_SingleKey(benchmark::State& s) {
  // HINT: one Lease with a huge token count; each thread calls TryTake in a loop.
  for (auto _ : s) { benchmark::DoNotOptimize(table.TryTake(lease, 1)); }
}
BENCHMARK(BM_TryTake_SingleKey)->Threads(1)->Threads(4)->Threads(16);

static void BM_TryTake_ManyKeys(benchmark::State& s);   // 10,000 keys, thread picks one at random
BENCHMARK(BM_TryTake_ManyKeys)->Threads(1)->Threads(4)->Threads(16);
```

The gap between one hot key and many keys measures contention on a cache line, which is the reason
for the sharding and alignment choices in Chapter 4.

## Weeks 7 and 8: the matrix

| Variable | Values |
| --- | --- |
| Engine | AllowAll, Strict, Leased, Composite |
| Instances | 1, 2, 4 |
| Chunk size cap | 1%, 5%, 10% of the limit |
| Key distribution | uniform over 1M keys, Zipf (a few hot keys), one hot key |
| Client concurrency | up to the plateau |

Hint: one script, `bench/run_matrix.sh`, should run the whole matrix and write one file per cell
into `results/`. If you can rerun the entire evaluation with one command, your advisor can too, and
that is part of the definition of done in the plan.

## Profiling

```bash
perf record -F 999 -g -p $(pidof rate_limiter_server) -- sleep 20
perf script | stackcollapse-perf.pl | flamegraph.pl > results/flame_leased.svg
```

Fix the top one or two bottlenecks the flamegraph shows, re-run the same benchmark, and report
before and after. That is all the spec asks for; do not optimise anything without a number.

## The load generator

See [loadgen/README.md](loadgen/README.md). Start with `ghz`; write the custom C++ client only if
`ghz` saturates before the server does.

## Done when

- [ ] Week 2: baseline JSON files and setup notes in `results/`.
- [ ] Week 3: lease microbenchmarks saved.
- [ ] Week 7: matrix and flamegraph saved; Chapter 5 tables generated from them.
