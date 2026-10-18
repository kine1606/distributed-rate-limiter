// Closed-loop gRPC load generator (bench/loadgen/README.md, option 2). Keeps channels x in_flight
// Check calls outstanding for a fixed time and prints one JSON line: calls/s and latency percentiles.
// Faster than ghz, so at high load the limiter, not the client, is the limit.
//   loadgen <target> <rule> <seconds> [channels=8] [in_flight_per_channel=128] [keys=10000] [warmup_s=5]
//           [batch=1]
// Each check picks cust:1..keys uniformly; a call has a 1 s deadline. Each channel is its own TCP
// connection. Latency is measured from send to reply (closed loop: Week 5 adds open-loop timing).
// batch > 1 sends CheckBatch calls of that many checks: calls_per_s counts calls, checks_per_s checks.
// rule "envoy:<domain>" loads envoyproxy/ratelimit instead: one descriptor {cust: N} per check, a
// batch as that many descriptors in one ShouldRateLimit call.
#include <grpcpp/grpcpp.h>

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

  std::string Key() { return "cust:" + std::to_string(std::uniform_int_distribution<int>(1, keys)(rng)); }

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: loadgen <target> <rule> <seconds> [channels] [in_flight] [keys] [warmup_s] [batch]\n");
    return 2;
  }
  const std::string target = argv[1], rule = argv[2];
  const int seconds = std::stoi(argv[3]);
  const int channels = argc > 4 ? std::stoi(argv[4]) : 8;
  const int in_flight = argc > 5 ? std::stoi(argv[5]) : 128;
  const int keys = argc > 6 ? std::stoi(argv[6]) : 10'000;
  const int warmup = argc > 7 ? std::stoi(argv[7]) : 5;
  const int batch = argc > 8 ? std::stoi(argv[8]) : 1;

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
