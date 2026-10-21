#pragma once
// The gRPC service: turns calls into engine calls and engine results into replies (callback API,
// never blocks a gRPC thread). Header-only so tests can run it in-process.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "metrics/check_metrics.h"
#include "rate_limiter.grpc.pb.h"
#include "ratelimiter/clock.h"
#include "ratelimiter/core.h"
#include "server/convert.h"

namespace ratelimiter {

// Shared state for one CheckBatch call: the RPC finishes when the last sub-check completes,
// which may happen on different threads once Redis refills are involved.
struct BatchState {
  BatchState(grpc::ServerUnaryReactor* r, std::size_t n, std::atomic<std::int64_t>& in_flight)
      : reactor(r), size(static_cast<std::int64_t>(n)), pending(n), in_flight(in_flight) {}
  grpc::ServerUnaryReactor* reactor;
  const std::int64_t size;
  std::atomic<std::size_t> pending;
  std::atomic<bool> unknown_rule{false};  // published to the last sub-check by `pending`
  std::atomic<std::int64_t>& in_flight;   // the service's: the batch leaves it all at once
};

inline grpc::Status UnknownRule() { return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown rule"); }
inline grpc::Status Overloaded() { return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "limiter overloaded"); }

// Backpressure (Week 5): at most `max_in_flight` checks wait for an answer at once (a batch counts
// each entry); beyond that a call is refused at once with RESOURCE_EXHAUSTED. Every queue behind
// the service (lease and plan waiters, the batcher, retries) holds admitted checks only, so this one
// bound keeps a Redis stall from growing memory without limit.
class RateLimiterService final : public v1::RateLimiter::CallbackService {
 public:
  // `metrics`, if set, must outlive the service; nullptr records nothing (no clock reads).
  explicit RateLimiterService(std::shared_ptr<RateLimiterCore> core, std::int64_t max_in_flight = 10'000,
                              CheckMetrics* metrics = nullptr)
      : core_(std::move(core)), max_in_flight_(max_in_flight), metrics_(metrics) {}

  std::int64_t InFlight() const { return in_flight_.load(std::memory_order_relaxed); }

  grpc::ServerUnaryReactor* Check(grpc::CallbackServerContext* ctx, const v1::CheckRequest* req,
                                  v1::CheckResponse* resp) override {
    auto* reactor = ctx->DefaultReactor();
    if (const char* bad = Invalid(*req)) {
      Count(metrics_ ? &metrics_->rejected_invalid : nullptr);
      reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, bad));
      return reactor;
    }
    if (!Admit(1)) {
      Count(metrics_ ? &metrics_->rejected_overload : nullptr);
      reactor->Finish(Overloaded());
      return reactor;
    }
    const Started started = Start(req->rule());
    core_->Check(ToDomain(*req), [this, reactor, resp, started](const CheckResult& r) {
      in_flight_.fetch_sub(1, std::memory_order_relaxed);
      Record(started, r);
      if (r.reason == Reason::kUnknownRule) {
        reactor->Finish(UnknownRule());
        return;
      }
      Fill(r, resp);  // a Redis outage is a reason in an OK response, not a status (see the proto)
      reactor->Finish(grpc::Status::OK);
    });
    return reactor;
  }

  grpc::ServerUnaryReactor* CheckBatch(grpc::CallbackServerContext* ctx,
                                       const v1::CheckBatchRequest* req,
                                       v1::CheckBatchResponse* resp) override {
    auto* reactor = ctx->DefaultReactor();
    const int n = req->requests_size();
    if (n == 0) {
      reactor->Finish(grpc::Status::OK);
      return reactor;
    }
    if (n > kMaxBatch) {
      reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "at most 1000 entries per batch"));
      return reactor;
    }
    for (const auto& r : req->requests()) {
      if (const char* bad = Invalid(r)) {
        reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, bad));
        return reactor;
      }
    }
    if (n > max_in_flight_) {  // could never be admitted: not a transient overload to retry
      Count(metrics_ ? &metrics_->rejected_invalid : nullptr);
      reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "batch larger than the server's in-flight limit"));
      return reactor;
    }
    if (!Admit(n)) {
      Count(metrics_ ? &metrics_->rejected_overload : nullptr);
      reactor->Finish(Overloaded());
      return reactor;
    }
    resp->mutable_responses()->Reserve(n);
    for (int i = 0; i < n; ++i) resp->add_responses();

    auto state = std::make_shared<BatchState>(reactor, static_cast<std::size_t>(n), in_flight_);
    for (int i = 0; i < n; ++i) {
      v1::CheckResponse* slot = resp->mutable_responses(i);  // distinct slot per sub-check
      const Started started = Start(req->requests(i).rule());
      core_->Check(ToDomain(req->requests(i)), [this, state, slot, started](const CheckResult& r) {
        Record(started, r);
        if (r.reason == Reason::kUnknownRule) {
          state->unknown_rule.store(true, std::memory_order_relaxed);
        } else {
          Fill(r, slot);
        }
        if (state->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          state->in_flight.fetch_sub(state->size, std::memory_order_relaxed);
          state->reactor->Finish(state->unknown_rule.load(std::memory_order_relaxed) ? UnknownRule()
                                                                                     : grpc::Status::OK);
        }
      });
    }
    return reactor;
  }

 private:
  struct Started {
    CheckMetrics::PerRule* rule = nullptr;  // nullptr: metrics off, or a rule not in the config
    std::int64_t ns = 0;
  };
  Started Start(std::string_view rule) {
    if (metrics_ == nullptr) return {};
    return {metrics_->Of(rule), SteadyNowNs()};
  }
  void Record(const Started& s, const CheckResult& r) {
    if (metrics_ == nullptr) return;
    if (r.reason == Reason::kUnknownRule || s.rule == nullptr) {
      metrics_->rejected_invalid.Add();
      return;
    }
    s.rule->by_reason[static_cast<std::size_t>(r.reason)].Add();
    s.rule->latency.Observe(SteadyNowNs() - s.ns);
  }
  static void Count(Counter* c) {
    if (c != nullptr) c->Add();
  }

  bool Admit(std::int64_t n) {
    if (in_flight_.fetch_add(n, std::memory_order_relaxed) + n <= max_in_flight_) return true;
    in_flight_.fetch_sub(n, std::memory_order_relaxed);
    return false;
  }

  std::shared_ptr<RateLimiterCore> core_;
  const std::int64_t max_in_flight_;
  CheckMetrics* const metrics_;
  std::atomic<std::int64_t> in_flight_{0};
};

}  // namespace ratelimiter
