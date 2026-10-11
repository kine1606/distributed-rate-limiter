# Metrics and logging

> Week 5 (start counting earlier where useful) · Flow step A13 · Status: not started

## What this module does

It lets you see what the limiter is doing without slowing it down. Metrics are counters and
histograms that Prometheus scrapes over HTTP; logs are for rare events only. The benchmark chapter
uses these metrics too: the lease hit ratio and batch sizes explain why the throughput numbers are
what they are.

## Metrics to export

| Name | Type | Labels | Why you need it |
| --- | --- | --- | --- |
| `rl_checks_total` | counter | `rule`, `plan`, `outcome` | Allowed, denied, overage per plan; the business view |
| `rl_check_latency_seconds` | histogram | `path` (lease, strict, deny_cache) | p50/p99 server-side per path |
| `rl_redis_latency_seconds` | histogram | `script` | Separates Redis time from limiter time |
| `rl_lease_hits_total`, `rl_lease_misses_total` | counter | | Lease hit ratio, the key throughput driver |
| `rl_lease_refills_total` | counter | | How often Redis is actually asked |
| `rl_batch_size` | histogram | | Whether pipelining actually batches |
| `rl_batch_flush_total` | counter | `reason` (size, time) | Tuning the flush interval |
| `rl_breaker_state` | gauge | | 0 closed, 1 half-open, 2 open |
| `rl_deny_cache_hits_total` | counter | | The flood test result |
| `rl_queue_depth` | gauge | `queue` | Backpressure visibility |
| `rl_rejected_total` | counter | `reason` (overload, invalid) | `RESOURCE_EXHAUSTED` count |
| `rl_usage_dropped_total` | counter | | Lost usage events |
| `rl_plan_cache_hits_total` | counter | | Plan cache effectiveness |

Hint: never put the customer ID in a label. With a million customers that is a million time
series, and Prometheus falls over. Per-customer numbers belong in usage events.

## Skeleton

```cpp
class Metrics {
 public:
  static Metrics& Get();                       // created once in main()
  void Expose(const std::string& addr);        // prometheus::Exposer on e.g. 0.0.0.0:9100
  prometheus::Counter& checks(std::string_view rule, std::string_view plan, Outcome o);
  prometheus::Histogram& check_latency(Path p);
  // ...one accessor per metric above
 private:
  std::shared_ptr<prometheus::Registry> registry_;
};
```

## Build it in this order

1. Expose a registry on a port and one counter; check it with `curl localhost:9100/metrics`.
2. Look up label combinations once at startup and keep references. Looking up a labelled counter
   on every request costs a map lookup and a lock inside prometheus-cpp.
3. Latency: take one `steady_clock::now()` at the start of `Check` and observe in the callback.
4. Add `docker/prometheus.yml` and a Prometheus container to compose; optionally Grafana for a
   screenshot in the thesis.

## Logging rules

Use spdlog with one logger. Log at startup (config, Redis nodes, script SHAs), on state changes
(breaker opens or closes, rule reload, failover), and sampled errors (at most one per second per
error type). Never log per request on the hot path; at 100,000 checks/s that is 100,000 lines per
second and the logging would become the bottleneck you measure.

## Done when

- [ ] Every metric in the table exported and visible in Prometheus.
- [ ] Benchmark with metrics on vs off shows the overhead is small (record the number).
