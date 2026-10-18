#pragma once
// The gRPC service: turns calls into engine calls and engine results into replies (callback API,
// never blocks a gRPC thread). Header-only so tests can run it in-process.

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <utility>

#include "rate_limiter.grpc.pb.h"
#include "ratelimiter/core.h"
#include "server/convert.h"

namespace ratelimiter {

// Shared state for one CheckBatch call: the RPC finishes when the last sub-check completes,
// which may happen on different threads once Redis refills are involved.
struct BatchState {
  BatchState(grpc::ServerUnaryReactor* r, std::size_t n) : reactor(r), pending(n) {}
  grpc::ServerUnaryReactor* reactor;
  std::atomic<std::size_t> pending;
  std::atomic<bool> unknown_rule{false};  // published to the last sub-check by `pending`
};

inline grpc::Status UnknownRule() { return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown rule"); }

class RateLimiterService final : public v1::RateLimiter::CallbackService {
 public:
  explicit RateLimiterService(std::shared_ptr<RateLimiterCore> core) : core_(std::move(core)) {}

  grpc::ServerUnaryReactor* Check(grpc::CallbackServerContext* ctx, const v1::CheckRequest* req,
                                  v1::CheckResponse* resp) override {
    auto* reactor = ctx->DefaultReactor();
    if (const char* bad = Invalid(*req)) {
      reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, bad));
      return reactor;
    }
    core_->Check(ToDomain(*req), [reactor, resp](const CheckResult& r) {
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
    resp->mutable_responses()->Reserve(n);
    for (int i = 0; i < n; ++i) resp->add_responses();

    auto state = std::make_shared<BatchState>(reactor, static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      v1::CheckResponse* slot = resp->mutable_responses(i);  // distinct slot per sub-check
      core_->Check(ToDomain(req->requests(i)), [state, slot](const CheckResult& r) {
        if (r.reason == Reason::kUnknownRule) {
          state->unknown_rule.store(true, std::memory_order_relaxed);
        } else {
          Fill(r, slot);
        }
        if (state->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          state->reactor->Finish(state->unknown_rule.load(std::memory_order_relaxed) ? UnknownRule()
                                                                                     : grpc::Status::OK);
        }
      });
    }
    return reactor;
  }

 private:
  std::shared_ptr<RateLimiterCore> core_;
};

}  // namespace ratelimiter
