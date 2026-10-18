# Benchmarks

> Week 2 (baseline), Week 3 (microbenchmarks), Weeks 7 and 8 (full matrix) · Status: Week 2 baseline done (`results/2026-10-17_baseline_NOTES.md`); Week 3 lease microbenchmarks done (`bench_lease.cc`, `results/2026-10-18_lease_microbench/NOTES.md`)

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
# 1 instance, StrictCore, Free customers spread over 10,000 keys. Flushes the dev cluster, seeds the
# plans, starts the server, warms up 10 s, measures 30 s; summary in results/<date>_<label>.txt
bench/ghz_run.sh strict 64 'cust:{{randomInt 1 10000}}' daily_quota baseline_strict_c64_r1
bench/ghz_run.sh allow_all 64 'cust:{{randomInt 1 10000}}' daily_quota baseline_allow_all_c64_r1
```

The script computes p50, p99 and p99.9 from every request in the ghz JSON (`details`), records the
CPU of the server, of the three Redis containers (cgroup counters) and of ghz, then gzips the JSON.

Hint: increase `-c` (concurrent workers) until throughput stops growing; that plateau is the
number to report, together with the latency at that point. Report p50, p99 and p999 from the ghz
JSON. Run once with `AllowAllCore` as well: the difference between AllowAll and Strict is the cost
of Redis, which is exactly what the lease removes.

## Week 3: microbenchmarks

```bash
build/release/bench/bench_lease --benchmark_repetitions=5 --benchmark_report_aggregates_only=true \
    --benchmark_out=results/<date>_lease_microbench/run.json --benchmark_out_format=json
```

| Benchmark | Measures |
| --- | --- |
| `BM_TakeCas_OneLease`, `BM_TakeFetchSub_OneLease` | contention on one lease: the CAS loop against `fetch_sub` with undo |
| `BM_OwnCounter<Packed>`, `BM_OwnCounter<Padded>` | false sharing: per-thread counters packed against one per cache line |
| `BM_FindAndTake/{1,10000}` | `LeaseTable::FindOrCreate` + `TryTake`, one hot key or 10,000 keys |
| `BM_LeasedCheckHit/{1,10000}` | the whole engine on a hit: rule lookup, lease lookup, take, callback |

Each runs at 1, 4 and 16 threads; `allocs/op` counts heap allocations during the timed loop (a hit
must show zero). Results and readings: `results/2026-10-18_lease_microbench/NOTES.md`. For a
before/after comparison on this noisy laptop, build both binaries and run them alternately, as that
file describes, rather than comparing against an older run.

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

- [x] Week 2: baseline JSON files and setup notes in `results/`.
- [x] Week 3: lease microbenchmarks saved.
- [ ] Week 7: matrix and flamegraph saved; Chapter 5 tables generated from them.
