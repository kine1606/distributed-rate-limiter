#pragma once
// Counters and histograms cheap enough for the request path, and their Prometheus text form
// (src/metrics/README.md). Written here instead of with prometheus-cpp, which is not installed: the
// text format is a few lines (prometheus.io/docs/instrumenting/exposition_formats).

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace ratelimiter {

// A counter split over cache lines, one per group of threads, so a busy server's threads rarely
// write the same line (the cost measured for shared atomics in Week 4). Value() adds them up.
class Counter {
 public:
  void Add(std::uint64_t n = 1) { cells_[Stripe()].v.fetch_add(n, std::memory_order_relaxed); }
  std::uint64_t Value() const {
    std::uint64_t sum = 0;
    for (const auto& c : cells_) sum += c.v.load(std::memory_order_relaxed);
    return sum;
  }

 private:
  static constexpr std::size_t kStripes = 16;
  static std::size_t Stripe() {
    static std::atomic<std::size_t> next{0};
    thread_local const std::size_t mine = next.fetch_add(1, std::memory_order_relaxed) % kStripes;
    return mine;
  }
  struct alignas(64) Cell {
    std::atomic<std::uint64_t> v{0};
  };
  std::array<Cell, kStripes> cells_;
};

// Latency in fixed buckets, in seconds as Prometheus expects: 50 µs to 1 s, then +Inf.
class Histogram {
 public:
  static constexpr std::array<double, 13> kBounds{0.00005, 0.0001, 0.00025, 0.0005, 0.001, 0.0025, 0.005,
                                                  0.01,    0.025,  0.05,    0.1,    0.25,  1.0};
  void Observe(std::int64_t ns) {
    const double s = static_cast<double>(ns) / 1e9;
    std::size_t i = 0;
    while (i < kBounds.size() && s > kBounds[i]) ++i;
    buckets_[i].Add();
    sum_ns_.Add(ns > 0 ? static_cast<std::uint64_t>(ns) : 0);
  }
  // Appends name_bucket (cumulative), name_sum and name_count; `labels` is `a="x",b="y"` or empty.
  void Render(std::string& out, std::string_view name, std::string_view labels) const;

 private:
  std::array<Counter, kBounds.size() + 1> buckets_;  // the last holds what is above every bound
  Counter sum_ns_;
};

// What the Redis client measures: latency per kind of call, and errors by kind (RedisPPBackend).
struct RedisMetrics {
  // Indexed like the backend's scripts: the four algorithms, then token returns, then plan reads.
  static constexpr std::array<std::string_view, 6> kCall{"token_bucket", "sliding_window", "fixed_window",
                                                         "quota",        "return",         "hgetall"};
  std::array<Histogram, kCall.size()> latency;
  Counter timeouts, reply_errors, other_errors;
  Counter reroutes;  // scripts sent again after MOVED or a lost node (a failover)
  void Render(std::string& out) const;
};

// `# HELP` and `# TYPE` lines of a metric family.
void AppendFamily(std::string& out, std::string_view name, std::string_view type, std::string_view help);
// One sample line: name{labels} value.
void AppendSample(std::string& out, std::string_view name, std::string_view labels, double value);
// A label value, escaped as the text format requires.
std::string LabelValue(std::string_view v);

}  // namespace ratelimiter
