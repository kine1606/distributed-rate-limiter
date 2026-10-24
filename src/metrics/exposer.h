#pragma once

#include <functional>
#include <string>
#include <thread>

namespace ratelimiter {

// Serves `GET /metrics` for Prometheus on a thread of its own, one connection at a time: scrapes
// come every few seconds, and nothing here is on the request path. Anything else gets 404.
class MetricsExposer {
 public:
  // `addr` is ip:port (port 0 picks a free one); throws std::runtime_error if it cannot listen.
  // `render` builds the body for each scrape.
  MetricsExposer(const std::string& addr, std::function<std::string()> render);
  ~MetricsExposer();

  MetricsExposer(const MetricsExposer&) = delete;
  MetricsExposer& operator=(const MetricsExposer&) = delete;

  int port() const { return port_; }

 private:
  void Run(std::stop_token stop);

  int fd_ = -1;
  int port_ = 0;
  const std::function<std::string()> render_;
  std::jthread thread_;  // last: starts once the socket listens
};

}  // namespace ratelimiter
