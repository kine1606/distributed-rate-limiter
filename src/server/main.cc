// gRPC server entry point (callback API, no synchronous handlers).
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

#include "config/rule_config.h"
#include "core/allow_all_core.h"
#include "rate_limiter.grpc.pb.h"
#include "ratelimiter/core.h"

namespace {

using namespace ratelimiter;  // NOLINT

std::atomic<bool> g_stop{false};
extern "C" void HandleSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

// Shared state for one CheckBatch call: the RPC finishes when the last sub-check completes,
// which may happen on different threads once Redis refills are involved.
struct BatchState {
  BatchState(grpc::ServerUnaryReactor* r, std::size_t n) : reactor(r), pending(n) {}
  grpc::ServerUnaryReactor* reactor;
  std::atomic<std::size_t> pending;
};

void Fill(const CheckResult& r, v1::CheckResponse* out) {
  out->set_allowed(r.allowed);
  out->set_remaining(r.remaining);
  out->set_retry_after_ms(r.retry_after_ms);
  out->set_limit(r.limit);
}

class RateLimiterService final : public v1::RateLimiter::CallbackService {
 public:
  explicit RateLimiterService(std::shared_ptr<RateLimiterCore> core) : core_(std::move(core)) {}

  grpc::ServerUnaryReactor* Check(grpc::CallbackServerContext* ctx, const v1::CheckRequest* req,
                                  v1::CheckResponse* resp) override {
    auto* reactor = ctx->DefaultReactor();
    if (req->key().empty() || req->rule().empty()) {
      reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "key and rule are required"));
      return reactor;
    }
    core_->Check(ToDomain(*req), [reactor, resp](const CheckResult& r) {
      Fill(r, resp);
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
    for (const auto& r : req->requests()) {
      if (r.key().empty() || r.rule().empty()) {
        reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "key and rule are required"));
        return reactor;
      }
    }
    resp->mutable_responses()->Reserve(n);
    for (int i = 0; i < n; ++i) resp->add_responses();

    auto state = std::make_shared<BatchState>(reactor, static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
      v1::CheckResponse* slot = resp->mutable_responses(i);  // distinct slot per sub-check
      core_->Check(ToDomain(req->requests(i)), [state, slot](const CheckResult& r) {
        Fill(r, slot);
        if (state->pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
          state->reactor->Finish(grpc::Status::OK);
        }
      });
    }
    return reactor;
  }

 private:
  static CheckRequest ToDomain(const v1::CheckRequest& r) {
    return CheckRequest{r.key(), r.rule(), r.cost() == 0 ? 1u : r.cost()};
  }

  std::shared_ptr<RateLimiterCore> core_;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string listen = argc > 1 ? argv[1] : "0.0.0.0:50051";
  const std::string rules_path = argc > 2 ? argv[2] : "config/rules.yaml";

  RuleTable rules;
  try {
    rules = LoadRules(rules_path);
  } catch (const std::exception& e) {
    spdlog::critical("cannot load rules from {}: {}", rules_path, e.what());
    return 1;
  }
  spdlog::info("loaded {} rules from {}", rules.size(), rules_path);

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  auto core = std::make_shared<AllowAllCore>();  // replaced by the real engine in later stages
  RateLimiterService service(core);

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    spdlog::critical("failed to start server on {}", listen);
    return 1;
  }
  server->GetHealthCheckService()->SetServingStatus(true);
  spdlog::info("rate limiter listening on {}", listen);

  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  spdlog::info("shutting down, draining in-flight requests");
  server->GetHealthCheckService()->SetServingStatus(false);
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
  server->Wait();
  return 0;
}
