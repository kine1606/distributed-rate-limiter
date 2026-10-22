// Closed-loop gRPC load generator (bench/loadgen/README.md, option 2). Keeps channels x in_flight
// Check calls outstanding for a fixed time and prints one JSON line: calls/s and latency percentiles.
// Faster than ghz, so at high load the limiter, not the client, is the limit.
//   loadgen <target> <rule> <seconds> [channels=8] [in_flight_per_channel=128] [keys=10000] [warmup_s=5]
//           [batch=1] [rate=0] [key_prefix=cust:]
// Each check picks cust:1..keys uniformly; a call has a 1 s deadline. Each channel is its own TCP
// connection. Latency is measured from send to reply (closed loop: Week 5 adds open-loop timing).
// batch > 1 sends CheckBatch calls of that many checks: calls_per_s counts calls, checks_per_s checks.
// rule "envoy:<domain>" loads envoyproxy/ratelimit instead: one descriptor {cust: N} per check, a
// batch as that many descriptors in one ShouldRateLimit call.
// rate > 0 runs open loop instead (Week 5): `rate` Check calls a second on a fixed schedule over the
// channels, whatever earlier calls are doing, and latency counts from each call's scheduled time, so
// a stall shows in the percentiles (no coordinated omission). in_flight per channel then caps the
// calls outstanding; a call the cap stops is counted as skipped, and any skip spoils the run. Open
// loop also counts answers by reason, in total and per second of the run (the fault tests' timeline).
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "rate_limiter.grpc.pb.h"
#include "rls_min.grpc.pb.h"

namespace {

using Clock = std::chrono::steady_clock;

// Log-scale latency buckets: 16 per doubling of microseconds (about 4% resolution), up to ~16 s.
constexpr int kBucketsPerDoubling = 16, kBuckets = 24 * kBucketsPerDoubling;
int Bucket(double us) {
  if (us < 1) return 0;
  return std::min(kBuckets - 1, static_cast<int>(std::log2(us) * kBucketsPerDoubling));
}
double BucketUpperUs(int b) { return std::exp2(static_cast<double>(b + 1) / kBucketsPerDoubling); }

std::atomic<bool> g_measuring{false}, g_stop{false};

// One outstanding call, reissued from its own completion: never more than one call at a time, so
// its counters and histogram need no lock.
struct Slot {
  ratelimiter::v1::RateLimiter::Stub* stub;
  envoy::service::ratelimit::v3::RateLimitService::Stub* rls = nullptr;  // set: load Envoy's service
  envoy::service::ratelimit::v3::RateLimitRequest rls_req;
  envoy::service::ratelimit::v3::RateLimitResponse rls_resp;
  std::string rule;
  int keys;
  std::mt19937_64 rng;
  std::unique_ptr<grpc::ClientContext> ctx;
  int batch = 1;
  ratelimiter::v1::CheckRequest req;
  ratelimiter::v1::CheckResponse resp;
  ratelimiter::v1::CheckBatchRequest breq;
  ratelimiter::v1::CheckBatchResponse bresp;
  Clock::time_point start;
  std::vector<std::uint64_t> hist = std::vector<std::uint64_t>(kBuckets);
  std::uint64_t ok = 0, checks = 0, allowed = 0, errors = 0;
  std::function<void()> on_finished;

  std::string prefix = "cust:";
  std::string Key() { return prefix + std::to_string(std::uniform_int_distribution<int>(1, keys)(rng)); }

  void Issue() {
    ctx = std::make_unique<grpc::ClientContext>();
    ctx->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(1));
    if (rls != nullptr) {
      rls_req.set_domain(rule);
      rls_req.clear_descriptors();
      for (int i = 0; i < batch; ++i) {
        auto* e = rls_req.add_descriptors()->add_entries();
        e->set_key("cust");
        e->set_value(std::to_string(std::uniform_int_distribution<int>(1, keys)(rng)));
      }
      start = Clock::now();
      rls->async()->ShouldRateLimit(ctx.get(), &rls_req, &rls_resp, [this](grpc::Status s) { Done(s); });
      return;
    }
    if (batch > 1) {
      breq.clear_requests();
      for (int i = 0; i < batch; ++i) {
        auto* r = breq.add_requests();
        r->set_key(Key());
        r->set_rule(rule);
      }
      start = Clock::now();
      stub->async()->CheckBatch(ctx.get(), &breq, &bresp, [this](grpc::Status s) { Done(s); });
      return;
    }
    req.set_key(Key());
    req.set_rule(rule);
    start = Clock::now();
    stub->async()->Check(ctx.get(), &req, &resp, [this](grpc::Status s) { Done(s); });
  }

  void Done(const grpc::Status& s) {
    if (g_measuring.load(std::memory_order_relaxed)) {
      ++hist[Bucket(std::chrono::duration<double, std::micro>(Clock::now() - start).count())];
      if (s.ok() && rls != nullptr) {
        ++ok;
        checks += static_cast<std::uint64_t>(batch);
        for (const auto& d : rls_resp.statuses()) allowed += d.code() == rls_resp.OK;
      } else if (s.ok() && batch > 1) {
        ++ok;
        checks += static_cast<std::uint64_t>(bresp.responses_size());
        for (const auto& r : bresp.responses()) allowed += r.allowed();
      } else if (s.ok()) {
        ++ok;
        ++checks;
        allowed += resp.allowed();
      } else {
        ++errors;
      }
    }
    if (g_stop.load(std::memory_order_relaxed)) {
      on_finished();
      return;
    }
    Issue();
  }
};

double Percentile(const std::vector<std::uint64_t>& h, double p) {
  std::uint64_t total = 0, seen = 0;
  for (auto c : h) total += c;
  for (int b = 0; b < kBuckets; ++b) {
    seen += h[b];
    if (static_cast<double>(seen) >= p * static_cast<double>(total)) return BucketUpperUs(b) / 1000.0;
  }
  return BucketUpperUs(kBuckets - 1) / 1000.0;
}

// ---- open loop ----

struct OpenCall {
  grpc::ClientContext ctx;
  ratelimiter::v1::CheckRequest req;
  ratelimiter::v1::CheckResponse resp;
  Clock::time_point scheduled;
  Clock::time_point measure_from;
  bool measured = false;
};

// Answers by proto Reason (0 to 7), then calls that failed (8).
constexpr int kOutcomes = 9;
constexpr std::array<const char*, kOutcomes> kOutcome{"unspecified", "ok",          "rate_limited",
                                                      "quota_exhausted", "overage", "suspended",
                                                      "unavailable", "unverified",  "error"};
struct OpenStats {
  explicit OpenStats(int seconds) : per_second(static_cast<std::size_t>(seconds)) {}
  std::array<std::atomic<std::uint64_t>, kBuckets> latency{}, lag{};  // from schedule; send minus schedule
  std::atomic<std::uint64_t> sent{0}, ok{0}, allowed{0}, errors{0}, skipped{0};
  std::atomic<std::int64_t> outstanding{0};
  std::array<std::atomic<std::uint64_t>, kOutcomes> outcomes{};
  std::array<std::atomic<std::uint64_t>, 17> error_codes{};  // failed calls by grpc::StatusCode
  std::vector<std::array<std::atomic<std::uint64_t>, kOutcomes>> per_second;  // by scheduled second
};

std::vector<std::uint64_t> Snapshot(const std::array<std::atomic<std::uint64_t>, kBuckets>& h) {
  std::vector<std::uint64_t> out(kBuckets);
  for (int b = 0; b < kBuckets; ++b) out[b] = h[b].load();
  return out;
}

double Us(Clock::duration d) { return std::chrono::duration<double, std::micro>(d).count(); }

// "outcomes":{...},"timeline":[[...], ...]: answers by kOutcome, in total and per second.
std::string Outcomes(const OpenStats& st) {
  std::string out = "\"outcomes\":{";
  for (int i = 1; i < kOutcomes; ++i) {
    out += std::string(i > 1 ? "," : "") + "\"" + kOutcome[static_cast<std::size_t>(i)] +
           "\":" + std::to_string(st.outcomes[static_cast<std::size_t>(i)].load());
  }
  out += "},\"error_codes\":{";  // e.g. 4 DEADLINE_EXCEEDED, 8 RESOURCE_EXHAUSTED, 14 UNAVAILABLE
  bool any = false;
  for (std::size_t code = 0; code < st.error_codes.size(); ++code) {
    if (const auto n = st.error_codes[code].load(); n > 0) {
      out += std::string(any ? "," : "") + "\"" + std::to_string(code) + "\":" + std::to_string(n);
      any = true;
    }
  }
  out += "},\"timeline_fields\":[";
  for (int i = 1; i < kOutcomes; ++i) out += std::string(i > 1 ? "," : "") + "\"" + kOutcome[static_cast<std::size_t>(i)] + "\"";
  out += "],\"timeline\":[";
  for (std::size_t sec = 0; sec < st.per_second.size(); ++sec) {
    out += sec > 0 ? ",[" : "[";
    for (int i = 1; i < kOutcomes; ++i) {
      out += (i > 1 ? "," : "") + std::to_string(st.per_second[sec][static_cast<std::size_t>(i)].load());
    }
    out += "]";
  }
  return out + "]";
}

// One sender per channel, each at rate / channels, staggered so the channels interleave.
int OpenLoop(std::vector<std::unique_ptr<ratelimiter::v1::RateLimiter::Stub>>& stubs, const std::string& rule,
             int seconds, int warmup, int keys, double rate, std::int64_t max_outstanding, const std::string& target,
             const std::string& prefix) {
  auto shared = std::make_shared<OpenStats>(seconds);  // callbacks may outlive this function's wait
  OpenStats& st = *shared;
  const auto n = static_cast<int>(stubs.size());
  const auto interval = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(n / rate));
  const auto start = Clock::now() + std::chrono::milliseconds(100);
  const auto measure_from = start + std::chrono::seconds(warmup);
  const auto stop_at = measure_from + std::chrono::seconds(seconds);
  std::vector<std::thread> senders;
  for (int i = 0; i < n; ++i) {
    senders.emplace_back([&, i] {
      std::mt19937_64 rng(static_cast<std::uint64_t>(i) + 1);
      for (auto t = start + interval * i / n; t < stop_at; t += interval) {
        std::this_thread::sleep_until(t);
        const bool measured = t >= measure_from;
        if (measured) ++st.lag[Bucket(Us(Clock::now() - t))];
        if (st.outstanding.load(std::memory_order_relaxed) >= max_outstanding) {
          if (measured) ++st.skipped;
          continue;
        }
        auto c = std::make_shared<OpenCall>();
        c->scheduled = t;
        c->measure_from = measure_from;
        c->measured = measured;
        c->ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
        c->req.set_key(prefix + std::to_string(std::uniform_int_distribution<int>(1, keys)(rng)));
        c->req.set_rule(rule);
        ++st.outstanding;
        if (measured) ++st.sent;
        stubs[static_cast<std::size_t>(i)]->async()->Check(&c->ctx, &c->req, &c->resp, [c, shared](grpc::Status s) {
          OpenStats& st = *shared;
          if (c->measured) {
            ++st.latency[Bucket(Us(Clock::now() - c->scheduled))];
            const int outcome = s.ok() ? std::clamp(static_cast<int>(c->resp.reason()), 0, 7) : 8;
            ++st.outcomes[static_cast<std::size_t>(outcome)];
            const auto second = static_cast<std::size_t>((c->scheduled - c->measure_from) / std::chrono::seconds(1));
            if (second < st.per_second.size()) ++st.per_second[second][static_cast<std::size_t>(outcome)];
            if (s.ok()) {
              ++st.ok;
              st.allowed += c->resp.allowed();
            } else {
              ++st.errors;
              ++st.error_codes[std::min<std::size_t>(static_cast<std::size_t>(s.error_code()), 16)];
            }
          }
          --st.outstanding;
        });
      }
    });
  }
  for (auto& t : senders) t.join();
  for (int i = 0; i < 300 && st.outstanding.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

  const auto lat = Snapshot(st.latency), lag = Snapshot(st.lag);
  std::printf(
      "{\"generator\":\"loadgen\",\"mode\":\"open\",\"target\":\"%s\",\"rule\":\"%s\",\"channels\":%d,"
      "\"max_outstanding\":%lld,\"keys\":%d,\"warmup_s\":%d,\"seconds\":%d,\"rate\":%.0f,"
      "\"sent_per_s\":%.0f,\"ok\":%llu,\"allowed\":%llu,\"errors\":%llu,\"skipped\":%llu,\"unanswered\":%lld,"
      "\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"p999_ms\":%.3f,\"send_lag_p99_ms\":%.3f,%s}\n",
      target.c_str(), rule.c_str(), n, static_cast<long long>(max_outstanding), keys, warmup, seconds, rate,
      static_cast<double>(st.sent.load()) / seconds, static_cast<unsigned long long>(st.ok.load()),
      static_cast<unsigned long long>(st.allowed.load()), static_cast<unsigned long long>(st.errors.load()),
      static_cast<unsigned long long>(st.skipped.load()), static_cast<long long>(st.outstanding.load()),
      Percentile(lat, 0.50), Percentile(lat, 0.99), Percentile(lat, 0.999), Percentile(lag, 0.99),
      Outcomes(st).c_str());
  return st.outstanding.load() > 0 ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr,
                 "usage: loadgen <target> <rule> <seconds> [channels] [in_flight] [keys] [warmup_s] [batch] [rate] "
                 "[key_prefix]\n");
    return 2;
  }
  const std::string target = argv[1], rule = argv[2];
  const int seconds = std::stoi(argv[3]);
  const int channels = argc > 4 ? std::stoi(argv[4]) : 8;
  const int in_flight = argc > 5 ? std::stoi(argv[5]) : 128;
  const int keys = argc > 6 ? std::stoi(argv[6]) : 10'000;
  const int warmup = argc > 7 ? std::stoi(argv[7]) : 5;
  const int batch = argc > 8 ? std::stoi(argv[8]) : 1;
  const double rate = argc > 9 ? std::stod(argv[9]) : 0;
  const std::string prefix = argc > 10 ? argv[10] : "cust:";

  const bool envoy = rule.starts_with("envoy:");
  std::vector<std::unique_ptr<ratelimiter::v1::RateLimiter::Stub>> stubs;
  std::vector<std::unique_ptr<envoy::service::ratelimit::v3::RateLimitService::Stub>> rls_stubs;
  for (int c = 0; c < channels; ++c) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);  // one TCP connection per channel
    auto channel = grpc::CreateCustomChannel(target, grpc::InsecureChannelCredentials(), args);
    stubs.push_back(ratelimiter::v1::RateLimiter::NewStub(channel));
    rls_stubs.push_back(envoy::service::ratelimit::v3::RateLimitService::NewStub(channel));
  }

  if (rate > 0) {
    return OpenLoop(stubs, rule, seconds, warmup, keys, rate, std::int64_t{channels} * in_flight, target, prefix);
  }

  std::mutex mu;
  std::condition_variable cv;
  int running = channels * in_flight;
  std::vector<std::unique_ptr<Slot>> slots;
  for (int i = 0; i < channels * in_flight; ++i) {
    auto s = std::make_unique<Slot>();
    s->stub = stubs[static_cast<std::size_t>(i % channels)].get();
    if (envoy) s->rls = rls_stubs[static_cast<std::size_t>(i % channels)].get();
    s->rule = envoy ? rule.substr(6) : rule;
    s->keys = keys;
    s->batch = batch;
    s->prefix = prefix;
    s->rng.seed(static_cast<std::uint64_t>(i) + 1);
    s->on_finished = [&] {
      std::lock_guard lock(mu);
      if (--running == 0) cv.notify_one();
    };
    slots.push_back(std::move(s));
  }
  for (auto& s : slots) s->Issue();

  std::this_thread::sleep_for(std::chrono::seconds(warmup));
  g_measuring = true;
  const auto t0 = Clock::now();
  std::this_thread::sleep_for(std::chrono::seconds(seconds));
  g_measuring = false;
  const double elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
  g_stop = true;
  {
    std::unique_lock lock(mu);
    cv.wait(lock, [&] { return running == 0; });
  }

  std::vector<std::uint64_t> hist(kBuckets);
  std::uint64_t ok = 0, checks = 0, allowed = 0, errors = 0;
  for (const auto& s : slots) {
    for (int b = 0; b < kBuckets; ++b) hist[b] += s->hist[b];
    ok += s->ok;
    checks += s->checks;
    allowed += s->allowed;
    errors += s->errors;
  }
  std::printf(
      "{\"generator\":\"loadgen\",\"target\":\"%s\",\"rule\":\"%s\",\"channels\":%d,\"in_flight\":%d,"
      "\"keys\":%d,\"warmup_s\":%d,\"batch\":%d,\"seconds\":%.1f,"
      "\"calls_per_s\":%.0f,\"checks_per_s\":%.0f,\"ok\":%llu,\"allowed\":%llu,\"errors\":%llu,"
      "\"p50_ms\":%.3f,\"p99_ms\":%.3f,\"p999_ms\":%.3f}\n",
      target.c_str(), rule.c_str(), channels, channels * in_flight, keys, warmup, batch, elapsed,
      static_cast<double>(ok) / elapsed, static_cast<double>(checks) / elapsed, static_cast<unsigned long long>(ok),
      static_cast<unsigned long long>(allowed),
      static_cast<unsigned long long>(errors), Percentile(hist, 0.50), Percentile(hist, 0.99),
      Percentile(hist, 0.999));
  return 0;
}
