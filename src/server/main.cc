// gRPC server entry point (callback API, no synchronous handlers).
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "config/rule_config.h"
#include "core/allow_all_core.h"
#include "core/leased_core.h"
#include "core/strict_core.h"
#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "redis/redispp_backend.h"
#include "server/service.h"

namespace {

using namespace ratelimiter;  // NOLINT

std::atomic<bool> g_stop{false};
extern "C" void HandleSignal(int) { g_stop.store(true, std::memory_order_relaxed); }

// Splits "host:port"; the port defaults to 6379.
std::pair<std::string, int> ParseHostPort(const std::string& addr) {
  const auto colon = addr.rfind(':');
  if (colon == std::string::npos) return {addr, 6379};
  return {addr.substr(0, colon), std::stoi(addr.substr(colon + 1))};
}

}  // namespace

// Usage: rate_limiter_server [listen] [rules.yaml] [redis cluster node host:port] [lua dir] [engine]
//   engine: leased (default; strict rules still go to Redis per check) | strict | allow_all (no
//   Redis; measures pure gRPC overhead)
int main(int argc, char** argv) {
  const std::string listen = argc > 1 ? argv[1] : "0.0.0.0:50051";
  const std::string rules_path = argc > 2 ? argv[2] : "config/rules.yaml";
  const std::string redis_addr = argc > 3 ? argv[3] : "127.0.0.1:7001";
  const std::string lua_dir = argc > 4 ? argv[4] : "lua";
  const std::string engine = argc > 5 ? argv[5] : "leased";

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

  std::unique_ptr<RedisPPBackend> redis;
  try {
    const auto [host, port] = ParseHostPort(redis_addr);
    redis = std::make_unique<RedisPPBackend>(host, port, lua_dir);
  } catch (const std::exception& e) {
    spdlog::critical("cannot set up redis backend: {}", e.what());
    return 1;
  }
  spdlog::info("using redis at {} with scripts from {}/", redis_addr, lua_dir);

  // RL_CHUNK_LOG=<file>: one CSV line per lease chunk change (ns,rule,key,from,to) for the
  // chunk-versus-demand plot. Changes happen at most once per refill, never on a hit.
  ChunkObserver chunk_log;
  if (const char* path = std::getenv("RL_CHUNK_LOG")) {
    std::shared_ptr<std::FILE> file(std::fopen(path, "a"), [](std::FILE* f) { if (f) std::fclose(f); });
    if (!file) {
      spdlog::critical("cannot open RL_CHUNK_LOG file {}", path);
      return 1;
    }
    chunk_log = [file, mu = std::make_shared<std::mutex>()](const Lease& l, std::uint32_t from, std::uint32_t to) {
      std::lock_guard lock(*mu);
      std::fprintf(file.get(), "%lld,%s,%s,%u,%u\n", static_cast<long long>(SteadyNowNs()), l.rule.name.c_str(),
                   l.key.c_str(), from, to);
      std::fflush(file.get());
    };
    spdlog::info("logging lease chunk changes to {}", path);
  }

  // Declared after `redis`, so they are destroyed first: nothing outlives the backend.
  PlanStore plans(*redis);
  std::shared_ptr<RateLimiterCore> core;
  if (engine == "leased") {
    core = std::make_shared<LeasedCore>(*redis, plans, std::move(rules), std::size_t{1} << 20, &SteadyNowNs,
                                        std::move(chunk_log));
  } else if (engine == "strict") {
    core = std::make_shared<StrictCore>(*redis, plans, std::move(rules));
  } else if (engine == "allow_all") {
    core = std::make_shared<AllowAllCore>();
  } else {
    spdlog::critical("unknown engine '{}' (leased | strict | allow_all)", engine);
    return 1;
  }
  spdlog::info("engine: {}", engine);
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
  redis.reset();  // stop Redis I/O while the core lives: an early refill may still be in flight
  return 0;
}
