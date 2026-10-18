#include "redis/plan_listener.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <spdlog/spdlog.h>
#include <sw/redis++/redis++.h>

namespace ratelimiter {

PlanListener::PlanListener(std::string host, int port, std::function<void(std::string_view)> on_change,
                           std::function<void()> on_resync)
    : host_(std::move(host)),
      port_(port),
      on_change_(std::move(on_change)),
      on_resync_(std::move(on_resync)),
      thread_([this](std::stop_token stop) { Run(stop); }) {}

void PlanListener::Run(std::stop_token stop) {
  sw::redis::ConnectionOptions o;
  o.host = host_;
  o.port = port_;
  o.connect_timeout = std::chrono::milliseconds(200);
  o.socket_timeout = std::chrono::milliseconds(200);  // consume() returns this often to see `stop`
  bool failing = false;
  while (!stop.stop_requested()) {
    try {
      sw::redis::RedisCluster cluster(o);
      auto sub = cluster.subscriber();  // a connection to one node, picked from the slot map
      sub.on_message([this](std::string, std::string customer) { on_change_(customer); });
      sub.subscribe(kChannel);
      sub.consume();  // the SUBSCRIBE confirmation: from here on no message is missed
      on_resync_();
      spdlog::info("subscribed to {}", kChannel);
      failing = false;
      while (!stop.stop_requested()) {
        try {
          sub.consume();
        } catch (const sw::redis::TimeoutError&) {  // no message for 200 ms; the connection is fine
        }
      }
    } catch (const sw::redis::Error& e) {
      if (!failing) spdlog::error("{} subscription lost, retrying every 500 ms: {}", kChannel, e.what());
      failing = true;
      std::mutex mu;
      std::unique_lock lock(mu);
      std::condition_variable_any().wait_for(lock, stop, std::chrono::milliseconds(500), [] { return false; });
    }
  }
}

}  // namespace ratelimiter
