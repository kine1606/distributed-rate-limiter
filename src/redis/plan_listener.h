#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <thread>

namespace ratelimiter {

// Subscribes to the plan-change channel (tools/rlctl publishes a customer ID there after writing
// the plan hash) on one thread of its own, reconnecting through any cluster node when the
// subscription breaks. In Redis Cluster a PUBLISH reaches subscribers on every node.
class PlanListener {
 public:
  static constexpr std::string_view kChannel = "plan-changes";

  // `on_change(customer)` runs for every message. `on_resync()` runs each time the subscription is
  // (re)established, because messages published while it was down are lost. Both run on the
  // listener thread and must be thread-safe. `host`:`port` is any cluster node.
  PlanListener(std::string host, int port, std::function<void(std::string_view)> on_change,
               std::function<void()> on_resync);

  PlanListener(const PlanListener&) = delete;
  PlanListener& operator=(const PlanListener&) = delete;

 private:
  void Run(std::stop_token stop);

  const std::string host_;
  const int port_;
  const std::function<void(std::string_view)> on_change_;
  const std::function<void()> on_resync_;
  std::jthread thread_;  // last: starts after the rest is set up; destruction stops it within about 200 ms
};

}  // namespace ratelimiter
