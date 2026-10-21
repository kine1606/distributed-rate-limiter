// gRPC server entry point (callback API, no synchronous handlers).
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <algorithm>
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
#include <vector>

#include <unistd.h>

#include <spdlog/spdlog.h>

#include "config/rule_config.h"
#include "core/allow_all_core.h"
#include "core/composite_core.h"
#include "core/leased_core.h"
#include "core/strict_core.h"
#include "lease/deny_cache.h"
#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "redis/batcher.h"
#include "redis/plan_listener.h"
#include "redis/redispp_backend.h"
#include "redis/sharded_backend.h"
#include "reliability/circuit_breaker.h"
#include "reliability/resilient_backend.h"
#include "server/service.h"
#include "usage/usage_log.h"

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
//   Redis; measures pure gRPC overhead). With leased and strict, customer rules (burst then quota,
//   config/rules.yaml) go through CompositeCore.
//   env: RL_PLAN_TTL_MS (plan cache, default 5000; 0 reads the plan on every check),
//        RL_REDIS_TIMEOUT_MS (each Redis call, default 50), RL_REDIS_LOOPS (Redis clients, default 4),
//        RL_MAX_IN_FLIGHT (checks waiting at once before RESOURCE_EXHAUSTED, default 10000),
//        RL_USAGE_FILE, RL_BATCHER, RL_BATCHER_DELAY_US, RL_CHUNK_LOG (below)
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

  // N Redis clients, each with its own I/O thread, keys spread over them: one thread that also
  // finishes every gRPC call it answers held the exact path to 8,896 checks/s at 192 in flight; four
  // gave 36,283. The timeout is the smallest that raised no false timeouts at that load (10 ms: 8 in
  // 8 s, each dropping a connection and its calls). results/2026-10-21_reliability/NOTES.md
  std::unique_ptr<RedisBackend> redis;
  const char* loops_env = std::getenv("RL_REDIS_LOOPS");
  const int loops = loops_env ? std::stoi(loops_env) : 4;
  const char* timeout_env = std::getenv("RL_REDIS_TIMEOUT_MS");
  const std::chrono::milliseconds timeout(timeout_env ? std::stoi(timeout_env) : 50);
  try {
    const auto [host, port] = ParseHostPort(redis_addr);
    std::vector<std::unique_ptr<RedisBackend>> shards;
    for (int i = 0; i < std::max(1, loops); ++i) {
      shards.push_back(std::make_unique<RedisPPBackend>(host, port, lua_dir, timeout));
    }
    redis = shards.size() == 1 ? std::move(shards.front()) : std::make_unique<ShardedBackend>(std::move(shards));
  } catch (const std::exception& e) {
    spdlog::critical("cannot set up redis backend: {}", e.what());
    return 1;
  }
  spdlog::info("using redis at {} with scripts from {}/, {} client(s), timeout {} ms", redis_addr, lua_dir,
               std::max(1, loops), timeout.count());

  // RL_BATCHER=timer|adaptive puts a Batcher in front of Redis (off by default: measured in
  // results/<date>_batcher/); RL_BATCHER_DELAY_US sets its timer (default 200).
  std::unique_ptr<Batcher> batcher;
  RedisBackend* backend = redis.get();
  if (const char* mode = std::getenv("RL_BATCHER")) {
    const std::string m = mode;
    if (m != "timer" && m != "adaptive") {
      spdlog::critical("RL_BATCHER must be timer or adaptive, not '{}'", m);
      return 1;
    }
    const char* delay = std::getenv("RL_BATCHER_DELAY_US");
    batcher = std::make_unique<Batcher>(*redis, m == "timer" ? Batcher::Mode::kTimer : Batcher::Mode::kAdaptive,
                                        std::chrono::microseconds(delay ? std::stoi(delay) : 200));
    backend = batcher.get();
    spdlog::info("batcher: {}, {} us", m, delay ? delay : "200");
  }

  // Everything above Redis goes through the breaker (src/reliability): 20 failures in a row stop
  // calls for 500 ms, then one probe; timed-out calls that cannot loosen a limit are retried twice.
  CircuitBreaker breaker;
  ResilientBackend resilient(*backend, breaker, {}, [](CircuitBreaker::State s) {
    static constexpr const char* kName[] = {"closed", "half-open", "open"};
    if (s == CircuitBreaker::State::kOpen) {
      spdlog::error("redis circuit breaker {}: calls fail at once, plan failure policies apply", kName[2]);
    } else {
      spdlog::info("redis circuit breaker {}", kName[static_cast<int>(s)]);
    }
  });
  backend = &resilient;

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

  // RL_USAGE_FILE=<file>: usage events, one JSON line per customer, minute and flush
  // (src/usage/README.md). Declared before the core, so it outlives every decision it records.
  std::unique_ptr<UsageLog> usage;
  UsageObserver on_decision;
  if (const char* path = std::getenv("RL_USAGE_FILE")) {
    char host[256]{};
    gethostname(host, sizeof host - 1);
    try {
      usage = std::make_unique<UsageLog>(path, host);
    } catch (const std::exception& e) {
      spdlog::critical("{}", e.what());
      return 1;
    }
    on_decision = [u = usage.get()](std::string_view c, Reason r, std::uint32_t cost) { u->Record(c, r, cost); };
    spdlog::info("writing usage events to {} as instance {}", path, host);
  }

  // Declared after `redis`, so they are destroyed first: nothing outlives the backend.
  const char* ttl = std::getenv("RL_PLAN_TTL_MS");
  PlanStore plans(*backend, std::chrono::milliseconds(ttl ? std::stoi(ttl) : 5000));
  DenyCache deny;
  // An announced plan change drops the cached plan and the local denial at once (step B5); the TTL
  // covers a message missed while the subscription was down.
  std::unique_ptr<PlanListener> listener;
  if (engine != "allow_all") {
    const auto [host, port] = ParseHostPort(redis_addr);
    listener = std::make_unique<PlanListener>(
        host, port,
        [&plans, &deny](std::string_view customer) {
          plans.Invalidate(customer);
          deny.Clear(customer);
        },
        [&plans] { plans.InvalidateAll(); });
  }
  StrictCore customer_strict(*backend, plans, rules);  // CompositeCore's burst and quota checks
  std::shared_ptr<RateLimiterCore> core;
  LeasedCore* leased = nullptr;  // for the shutdown statistics
  if (engine == "leased") {
    auto inner = std::make_shared<LeasedCore>(*backend, plans, rules, std::size_t{1} << 20, &SteadyNowNs,
                                              std::move(chunk_log));
    leased = inner.get();
    core = std::make_shared<CompositeCore>(std::move(inner), customer_strict, plans, deny, std::move(rules),
                                           &SteadyNowNs, std::move(on_decision), leased);  // leased quota too
  } else if (engine == "strict") {
    core = std::make_shared<CompositeCore>(std::make_shared<StrictCore>(*backend, plans, rules), customer_strict, plans,
                                           deny, std::move(rules), &SteadyNowNs, std::move(on_decision));
  } else if (engine == "allow_all") {
    core = std::make_shared<AllowAllCore>();
  } else {
    spdlog::critical("unknown engine '{}' (leased | strict | allow_all)", engine);
    return 1;
  }
  spdlog::info("engine: {}, plan cache {} ms", engine, ttl ? ttl : "5000");
  const char* max_in_flight = std::getenv("RL_MAX_IN_FLIGHT");
  RateLimiterService service(core, max_in_flight ? std::stoll(max_in_flight) : 10'000);

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

  // Graceful shutdown (Week 5): health NOT_SERVING so load balancers stop sending, drain for up to
  // 5 s, give unused leased tokens back, then stop Redis. Usage events flush as `usage` goes.
  spdlog::info("shutting down, draining {} in-flight checks", service.InFlight());
  server->GetHealthCheckService()->SetServingStatus(false);
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
  server->Wait();
  if (leased != nullptr) {
    const auto s = leased->GetStats();
    spdlog::info("lease stats: {} checks waited for a refill, {} refills sent", s.parked, s.refills);
    const auto r = leased->ReturnLeases(std::chrono::seconds(1));
    spdlog::info("returned {} unused tokens of {} leases to Redis ({} returns failed)", r.tokens, r.leases,
                 r.failed);
  }
  if (usage && usage->Dropped() > 0) spdlog::warn("usage events: {} dropped", usage->Dropped());
  resilient.StopRetries();  // no retry may reach Redis after this
  batcher.reset();  // sends what it holds and waits for the replies, while Redis is still there
  redis.reset();    // stop Redis I/O while the core lives: an early refill may still be in flight
  return 0;
}
