#pragma once
// The credentials the limiter gives Redis, for a cluster that requires AUTH or an ACL user (Redis 6+):
// RL_REDIS_USER and RL_REDIS_PASSWORD. Unset, none: then Redis must be reachable only from a private
// network. Read here, where every connection is made (the backend and the plan listener), so no
// connection can be left without them.
#include <cstdlib>

#include <sw/redis++/connection.h>

namespace ratelimiter {

inline void SetRedisAuth(sw::redis::ConnectionOptions& o) {
  if (const char* user = std::getenv("RL_REDIS_USER"); user != nullptr && *user != '\0') o.user = user;
  if (const char* password = std::getenv("RL_REDIS_PASSWORD"); password != nullptr && *password != '\0') {
    o.password = password;
  }
}

}  // namespace ratelimiter
