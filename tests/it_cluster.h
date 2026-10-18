#pragma once
// Shared by the integration tests: where the dev cluster is, and an admin connection to it.

#include <sw/redis++/redis++.h>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace ratelimiter {

// env RL_TEST_CLUSTER="host:port", default 127.0.0.1:7001 (docker/docker-compose.dev.yml).
inline std::pair<std::string, int> ClusterSeed() {
  const char* env = std::getenv("RL_TEST_CLUSTER");
  const std::string a = env ? env : "127.0.0.1:7001";
  const auto colon = a.rfind(':');
  return {a.substr(0, colon), std::stoi(a.substr(colon + 1))};
}

// nullptr when the cluster does not answer, so the test can skip.
inline std::unique_ptr<sw::redis::RedisCluster> ConnectAdmin() {
  try {
    sw::redis::ConnectionOptions o;
    o.host = ClusterSeed().first;
    o.port = ClusterSeed().second;
    o.connect_timeout = std::chrono::milliseconds(200);
    o.socket_timeout = std::chrono::seconds(2);
    auto c = std::make_unique<sw::redis::RedisCluster>(o);
    c->exists("{it}:probe");
    return c;
  } catch (const std::exception&) {
    return nullptr;
  }
}

}  // namespace ratelimiter
