// gRPC server entry point (callback API, no synchronous handlers).
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include <absl/synchronization/mutex.h>
#include <spdlog/spdlog.h>

#include "config/rule_config.h"
#include "core/allow_all_core.h"
#include "core/composite_core.h"
#include "core/leased_core.h"
#include "core/strict_core.h"
#include "lease/deny_cache.h"
#include "metrics/check_metrics.h"
#include "metrics/exposer.h"
#include "metrics/metrics.h"
#include "plan/plan_store.h"
#include "ratelimiter/core.h"
#include "redis/batcher.h"
#include "redis/plan_listener.h"
#include "redis/redispp_backend.h"
#include "redis/sharded_backend.h"
#include "reliability/resilient_backend.h"
#include "reliability/slot_breakers.h"
#include "server/service.h"
#include "usage/usage_log.h"

namespace {

using namespace ratelimiter;  // NOLINT

std::atomic<bool> g_stop{false}, g_reload{false};
extern "C" void HandleSignal(int) { g_stop.store(true, std::memory_order_relaxed); }
extern "C" void HandleReload(int) { g_reload.store(true, std::memory_order_relaxed); }

// A numeric setting from the environment: `fallback` when unset, nullopt (logged) when it is not a
// whole number from `min` to `max`. A bad value must stop the start, not become a silent default.
std::optional<long long> EnvNumber(const char* name, long long fallback, long long min, long long max) {
  const char* v = std::getenv(name);
  if (v == nullptr) return fallback;
  long long n = 0;
  const char* last = v + std::strlen(v);
  const auto [end, ec] = std::from_chars(v, last, n);
  if (ec != std::errc{} || end != last || n < min || n > max) {
    spdlog::critical("{}='{}' must be a whole number from {} to {}", name, v, min, max);
    return std::nullopt;
  }
  return n;
}

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
//        RL_METRICS_ADDR (Prometheus endpoint ip:port, default 127.0.0.1:9100; off disables it),
//        RL_MAX_STREAMS (concurrent calls per client connection, default 100; 0: unlimited),
//        RL_USAGE_FILE, RL_BATCHER, RL_BATCHER_DELAY_US, RL_CHUNK_LOG (below)
//   SIGHUP reloads rules.yaml (a file that does not load keeps the old rules); SIGTERM drains and stops.
int main(int argc, char** argv) {
  // gRPC's mutexes are abseil's, and Ubuntu's abseil checks every lock for deadlock cycles: 2.7% of
  // the server's CPU went to GraphCycles alone under load (perf, 2026-10-23). A server needs no such check.
  absl::SetMutexDeadlockDetectionMode(absl::OnDeadlockCycle::kIgnore);
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

  // Metrics (src/metrics): recorded on the request path unless RL_METRICS_ADDR=off. Declared before
  // everything that records into them, so they outlive it.
  const char* metrics_env = std::getenv("RL_METRICS_ADDR");
  const std::string metrics_addr = metrics_env ? metrics_env : "127.0.0.1:9100";
  const bool metrics_on = metrics_addr != "off";
  CheckMetrics check_metrics(rules);
  RedisMetrics redis_metrics;

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  std::signal(SIGHUP, HandleReload);

  // N Redis clients, each with its own I/O thread, keys spread over them: one thread that also
  // finishes every gRPC call it answers held the exact path to 8,896 checks/s at 192 in flight; four
  // gave 36,283. The timeout is the smallest that raised no false timeouts at that load (10 ms: 8 in
  // 8 s, each dropping a connection and its calls). results/2026-10-21_reliability/NOTES.md
  std::unique_ptr<RedisBackend> redis;
  const auto loops = EnvNumber("RL_REDIS_LOOPS", 4, 1, 64);
  const auto timeout_ms = EnvNumber("RL_REDIS_TIMEOUT_MS", 50, 1, 60'000);  // 0 would mean no timeout at all
  const auto batcher_us = EnvNumber("RL_BATCHER_DELAY_US", 200, 1, 1'000'000);
  const auto plan_ttl_ms = EnvNumber("RL_PLAN_TTL_MS", 5000, 0, 3'600'000);
  const auto max_in_flight = EnvNumber("RL_MAX_IN_FLIGHT", 10'000, 1, 100'000'000);
  // Concurrent calls per client connection. Unlimited (gRPC's default, 0 here), an open-loop load past
  // capacity piled up thousands of streams and collapsed even with no limiter (allow_all, 30,000/s:
  // p99 2 s, calls lost); capped at 100, the same load queued and drained (p99 137 ms). The cost: a
  // client on one connection with 192 calls in flight on the exact path, 28,251 to 22,957 checks/s.
  const auto max_streams = EnvNumber("RL_MAX_STREAMS", 100, 0, 1'000'000);
  if (!loops || !timeout_ms || !batcher_us || !plan_ttl_ms || !max_in_flight || !max_streams) return 1;
  const std::chrono::milliseconds timeout(*timeout_ms);
  try {
    const auto [host, port] = ParseHostPort(redis_addr);
    std::vector<std::unique_ptr<RedisBackend>> shards;
    for (long long i = 0; i < *loops; ++i) {
      shards.push_back(
          std::make_unique<RedisPPBackend>(host, port, lua_dir, timeout, metrics_on ? &redis_metrics : nullptr));
    }
    redis = shards.size() == 1 ? std::move(shards.front()) : std::make_unique<ShardedBackend>(std::move(shards));
  } catch (const std::exception& e) {
    spdlog::critical("cannot set up redis backend: {}", e.what());
    return 1;
  }
  spdlog::info("using redis at {} with scripts from {}/, {} client(s), timeout {} ms", redis_addr, lua_dir,
               *loops, timeout.count());

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
    batcher = std::make_unique<Batcher>(*redis, m == "timer" ? Batcher::Mode::kTimer : Batcher::Mode::kAdaptive,
                                        std::chrono::microseconds(*batcher_us));
    backend = batcher.get();
    spdlog::info("batcher: {}, {} us", m, *batcher_us);
  }

  // Everything above Redis goes through the breaker (src/reliability): 20 failures in a row stop
  // calls for 500 ms, then one probe; timed-out calls that cannot loosen a limit are retried twice.
  // The timeout is also each attempt's deadline: the client's own is an idle timeout per connection,
  // which a master answering every call late under load never trips.
  SlotBreakers breakers;
  ResilientBackend resilient(*backend, breakers, {.deadline = timeout}, [](std::size_t open) {
    static std::atomic<bool> any_open{false};  // log the start and the end of an outage, not each group
    if ((open > 0) == any_open.exchange(open > 0)) return;
    if (open > 0) {
      spdlog::error("redis circuit breakers open for {} of {} slot groups: their calls fail at once, plan "
                    "failure policies apply", open, SlotBreakers::kGroups);
    } else {
      spdlog::info("redis circuit breakers all closed again");
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
  PlanStore plans(*backend, std::chrono::milliseconds(*plan_ttl_ms));
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
  // One engine per rule table: a reload builds a new one from the new file (see the main loop).
  struct Engine {
    std::unique_ptr<StrictCore> customer_strict;  // CompositeCore's burst and quota checks
    std::shared_ptr<RateLimiterCore> core;
    LeasedCore* leased = nullptr;  // for the statistics and the lease returns
  };
  if (engine != "leased" && engine != "strict" && engine != "allow_all") {
    spdlog::critical("unknown engine '{}' (leased | strict | allow_all)", engine);
    return 1;
  }
  const auto make_engine = [&](RuleTable table) {
    auto e = std::make_unique<Engine>();
    if (engine == "allow_all") {
      e->core = std::make_shared<AllowAllCore>();
      return e;
    }
    e->customer_strict = std::make_unique<StrictCore>(*backend, plans, table);
    std::shared_ptr<RateLimiterCore> inner;
    if (engine == "leased") {
      auto l = std::make_shared<LeasedCore>(*backend, plans, table, std::size_t{1} << 20, &SteadyNowNs, chunk_log);
      e->leased = l.get();
      inner = std::move(l);
    } else {
      inner = std::make_shared<StrictCore>(*backend, plans, table);
    }
    e->core = std::make_shared<CompositeCore>(std::move(inner), *e->customer_strict, plans, deny, std::move(table),
                                              &SteadyNowNs, on_decision, e->leased);  // leased quota too
    return e;
  };
  // Every engine made, current last. A replaced one is kept until exit, like the service keeps it: a
  // check may still be inside it. Its lease statistics stay in the totals.
  std::vector<std::unique_ptr<Engine>> engines;
  std::mutex engines_mu;  // the reload adds, the metrics thread reads
  engines.push_back(make_engine(std::move(rules)));
  const auto lease_stats = [&] {
    LeasedCore::Stats total{0, 0};
    std::lock_guard lock(engines_mu);
    for (const auto& e : engines) {
      if (e->leased == nullptr) continue;
      const auto s = e->leased->GetStats();
      total.parked += s.parked;
      total.refills += s.refills;
    }
    return total;
  };
  spdlog::info("engine: {}, plan cache {} ms, at most {} checks in flight", engine, *plan_ttl_ms, *max_in_flight);
  RateLimiterService service(engines.back()->core, *max_in_flight, metrics_on ? &check_metrics : nullptr);

  // After everything it reads, so it stops first. A port in use (several servers on one machine)
  // leaves the server running without an endpoint.
  std::unique_ptr<MetricsExposer> exposer;
  if (metrics_on) {
    try {
      exposer = std::make_unique<MetricsExposer>(metrics_addr, [&] {
        std::string out;
        check_metrics.Render(out);
        redis_metrics.Render(out);
        AppendFamily(out, "rl_in_flight", "gauge", "Checks admitted and not answered yet");
        AppendSample(out, "rl_in_flight", "", static_cast<double>(service.InFlight()));
        AppendFamily(out, "rl_breaker_open_groups", "gauge", "Slot groups (of 64) whose Redis breaker is not closed");
        AppendSample(out, "rl_breaker_open_groups", "", static_cast<double>(breakers.Open()));
        AppendFamily(out, "rl_redis_deadline_misses_total", "counter", "Redis calls answered as timed out at their deadline");
        AppendSample(out, "rl_redis_deadline_misses_total", "", static_cast<double>(resilient.DeadlinesMissed()));
        if (engine == "leased") {
          const auto s = lease_stats();
          AppendFamily(out, "rl_lease_waits_total", "counter", "Checks that waited for a lease refill");
          AppendSample(out, "rl_lease_waits_total", "", static_cast<double>(s.parked));
          AppendFamily(out, "rl_lease_refills_total", "counter", "Lease refills sent to Redis");
          AppendSample(out, "rl_lease_refills_total", "", static_cast<double>(s.refills));
        }
        AppendFamily(out, "rl_plan_cache_reads_total", "counter", "Plan reads sent to Redis (cache misses)");
        AppendSample(out, "rl_plan_cache_reads_total", "", static_cast<double>(plans.Reads()));
        AppendFamily(out, "rl_deny_cache_hits_total", "counter", "Checks denied by the deny cache, no Redis call");
        AppendSample(out, "rl_deny_cache_hits_total", "", static_cast<double>(deny.Hits()));
        if (usage) {
          AppendFamily(out, "rl_usage_dropped_total", "counter", "Usage events dropped (buffer full or write failed)");
          AppendSample(out, "rl_usage_dropped_total", "", static_cast<double>(usage->Dropped()));
        }
        return out;
      });
      spdlog::info("metrics on http://{}:{}/metrics", metrics_addr.substr(0, metrics_addr.rfind(':')),
                   exposer->port());
    } catch (const std::exception& e) {
      spdlog::warn("metrics endpoint off: {}", e.what());
    }
  }

  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();

  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials());
  if (*max_streams > 0) builder.AddChannelArgument(GRPC_ARG_MAX_CONCURRENT_STREAMS, static_cast<int>(*max_streams));
  builder.RegisterService(&service);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server) {
    spdlog::critical("failed to start server on {}", listen);
    return 1;
  }
  server->GetHealthCheckService()->SetServingStatus(true);
  spdlog::info("rate limiter listening on {}", listen);

  // Readiness follows the breakers: NOT_SERVING while every slot group's breaker is open, that is
  // while this instance reaches no Redis master, so a load balancer prefers instances that do. One
  // dead master is not a reason: every instance sees it, and the plans' failure policies cover it.
  bool ready = true;
  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (g_reload.exchange(false, std::memory_order_relaxed)) {
      // SIGHUP (src/config/README.md): parse the whole file first; one that does not load leaves the
      // old rules live. Checks from the swap on use the new engine; checks already in the old one
      // finish there. Once they are done (a few Redis timeouts at most), the old engine's unused
      // leased tokens go back to Redis, as at shutdown. A check later still is no harm: tokens it
      // leases stay unused, which is stricter, never looser.
      RuleTable next;
      try {
        next = LoadRules(rules_path);
      } catch (const std::exception& e) {
        spdlog::error("reload: cannot load rules from {}: {}; the old rules stay live", rules_path, e.what());
        continue;
      }
      const std::size_t n = next.size();
      check_metrics.AddRules(next);
      auto fresh = make_engine(std::move(next));
      service.SetCore(fresh->core);
      LeasedCore* const old = engines.back()->leased;
      {
        std::lock_guard lock(engines_mu);
        engines.push_back(std::move(fresh));
      }
      spdlog::info("reload: {} rules from {} live", n, rules_path);
      if (old != nullptr) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        const auto r = old->ReturnLeases(std::chrono::seconds(1));
        spdlog::info("reload: the old engine returned {} unused tokens of {} leases ({} returns failed)", r.tokens,
                     r.leases, r.failed + r.refills_unfinished);
      }
    }
    if (const bool now = breakers.Open() < SlotBreakers::kGroups; now != ready) {
      ready = now;
      server->GetHealthCheckService()->SetServingStatus(ready);
      spdlog::warn("health: {}", ready ? "SERVING again" : "NOT_SERVING, no Redis master reachable");
    }
  }

  // Graceful shutdown (Week 5): health NOT_SERVING so load balancers stop sending, drain for up to
  // 5 s, give unused leased tokens back, then stop Redis. Usage events flush as `usage` goes.
  spdlog::info("shutting down, draining {} in-flight checks", service.InFlight());
  server->GetHealthCheckService()->SetServingStatus(false);
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
  server->Wait();
  if (LeasedCore* const leased = engines.back()->leased) {
    const auto s = lease_stats();
    spdlog::info("lease stats: {} checks waited for a refill, {} refills sent", s.parked, s.refills);
    const auto r = leased->ReturnLeases(std::chrono::seconds(1));
    spdlog::info("returned {} unused tokens of {} leases to Redis ({} returns failed, {} refills unfinished)",
                 r.tokens, r.leases, r.failed, r.refills_unfinished);
  }
  if (usage && usage->Dropped() > 0) spdlog::warn("usage events: {} dropped", usage->Dropped());
  resilient.StopRetries();  // no retry may reach Redis after this
  batcher.reset();  // sends what it holds and waits for the replies, while Redis is still there
  redis.reset();    // stop Redis I/O while the core lives: an early refill may still be in flight
  return 0;
}
