#include "redis/redispp_backend.h"

#include <openssl/sha.h>

#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "ratelimiter/clock.h"
#include "redis/keys.h"

namespace ratelimiter {
namespace {

using Reply = std::vector<long long>;
constexpr double kDayMs = 86'400'000.0;

std::string Sha1Hex(const std::string& s) {
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char*>(s.data()), s.size(), digest);
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  for (const unsigned char b : digest) {
    out += kHex[b >> 4];
    out += kHex[b & 15];
  }
  return out;
}

// ARGV in the order each script expects (contracts in lua/README.md).
std::vector<std::string> Args(const Rule& rule, std::uint32_t requested) {
  switch (rule.algorithm) {
    case Algorithm::kTokenBucket: {
      // An idle bucket is full again after capacity / rate, so expiring it then loses nothing
      // (+1 s for rounding). A plan with rate 0 never refills: keep its bucket for a day.
      const double full_after_ms = rule.refill_per_sec > 0 ? rule.limit / rule.refill_per_sec * 1000.0 : kDayMs;
      const auto ttl_ms = static_cast<std::int64_t>(std::ceil(full_after_ms)) + 1000;
      return {std::to_string(rule.limit), std::to_string(rule.refill_per_sec), std::to_string(requested),
              std::to_string(ttl_ms)};
    }
    case Algorithm::kSlidingWindow:
    case Algorithm::kFixedWindow:  // both scripts compute their own TTL from the window
      return {std::to_string(rule.limit), std::to_string(rule.window_ms), std::to_string(requested)};
    case Algorithm::kQuota: {
      static constexpr std::array<const char*, 3> kPolicy{"hard", "grace", "cap"};  // indexed by Overage
      return {std::to_string(rule.limit), std::to_string(requested), kPolicy[static_cast<std::size_t>(rule.overage)],
              std::to_string(rule.overage_extra)};
    }
  }
  return {};
}

ScriptResult Parse(Algorithm algorithm, const Reply& v) {
  ScriptResult r;
  if (algorithm == Algorithm::kQuota) {  // {allowed, used, remaining, reset_after_ms, overage, replayed}
    if (v.size() != 6) {
      r.redis_error = true;
      return r;
    }
    r.allowed = v[0] == 1;
    r.remaining = static_cast<std::uint32_t>(v[2]);
    r.reset_after_ms = static_cast<std::uint32_t>(v[3]);
    r.retry_after_ms = r.allowed ? 0 : r.reset_after_ms;
    r.overage = v[4] == 1;
    r.replayed = v[5] == 1;
    return r;
  }
  if (v.size() != 3) {  // {allowed, remaining, retry_after_ms}
    r.redis_error = true;
    return r;
  }
  r.allowed = v[0] == 1;
  r.remaining = static_cast<std::uint32_t>(v[1]);
  r.retry_after_ms = static_cast<std::uint32_t>(v[2]);
  return r;
}

sw::redis::ConnectionOptions Options(const std::string& host, int port, std::chrono::milliseconds timeout) {
  sw::redis::ConnectionOptions o;
  o.host = host;
  o.port = port;
  o.connect_timeout = timeout;  // no longer than a call: a dead node must fail fast, not on connect
  o.socket_timeout = timeout;   // also bounds each async command
  return o;
}

sw::redis::ConnectionPoolOptions PoolOptions() {
  sw::redis::ConnectionPoolOptions p;  // one connection per master: the spike showed more do not help
  p.wait_timeout = std::chrono::milliseconds(100);  // never park a caller indefinitely on a broken pool
  return p;
}

}  // namespace

std::array<RedisPPBackend::Script, 5> RedisPPBackend::LoadScripts(const std::string& lua_dir) {
  static constexpr std::array<const char*, 5> kFiles{"token_bucket.lua", "sliding_window.lua", "fixed_window.lua",
                                                     "quota.lua", "return_lease.lua"};  // indexed as scripts_
  std::array<RedisPPBackend::Script, 5> out;
  for (std::size_t i = 0; i < kFiles.size(); ++i) {
    const std::string path = lua_dir + "/" + kFiles[i];
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::stringstream ss;
    ss << f.rdbuf();
    out[i] = {ss.str(), Sha1Hex(ss.str())};
  }
  return out;
}

struct RedisPPBackend::Call {
  std::vector<std::string> keys;  // the first routes the call; all share its hash tag
  std::vector<std::string> args;
  Algorithm algorithm;  // shapes the reply
  std::size_t script;   // index into scripts_ (and RedisMetrics::kCall)
  ScriptCallback done;
  std::int64_t start_ns = 0;  // first send, for the latency metric
  bool rerouted = false;      // sent again after a failover once already
};

RedisPPBackend::RedisPPBackend(const std::string& host, int port, const std::string& lua_dir,
                               std::chrono::milliseconds timeout, RedisMetrics* metrics)
    : scripts_(LoadScripts(lua_dir)), metrics_(metrics), cluster_(Options(host, port, timeout), PoolOptions()) {
  // The slot map loads in the background. A routed call waits for it, so the first scripts, which
  // go straight to a key's node, are not sent to the seed node by mistake.
  auto ready = cluster_.exists("{rl}:startup");
  try {
    if (ready.wait_for(std::chrono::seconds(2)) != std::future_status::ready) throw std::runtime_error("timeout");
    ready.get();
    spdlog::info("redis cluster at {}:{} ready", host, port);
  } catch (const std::exception& e) {
    down_ = true;
    spdlog::error("redis cluster at {}:{} not answering at start: {}", host, port, e.what());
  }
}

void RedisPPBackend::Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                          std::string_view request_id, ScriptCallback done) {
  std::vector<std::string> keys{RedisKey(rule, key)};
  if (rule.algorithm == Algorithm::kQuota && !request_id.empty()) keys.push_back(RequestKey(rule, key, request_id));
  Send(std::make_shared<Call>(Call{std::move(keys), Args(rule, requested), rule.algorithm,
                                   static_cast<std::size_t>(rule.algorithm), std::move(done)}),
       false);
}

void RedisPPBackend::Return(const Rule& rule, std::string_view key, std::uint32_t tokens, ScriptCallback done) {
  Send(std::make_shared<Call>(Call{{RedisKey(rule, key)}, {std::to_string(rule.limit), std::to_string(tokens)},
                                   Algorithm::kTokenBucket, kReturnScript, std::move(done)}),
       false);
}

void RedisPPBackend::Send(const std::shared_ptr<Call>& c, bool with_source) {
  const Script& script = scripts_[c->script];
  if (metrics_ != nullptr && c->start_ns == 0) c->start_ns = SteadyNowNs();
  auto on_reply = [this, c, with_source](sw::redis::Future<Reply>&& f) {
    ScriptResult r;
    try {
      r = Parse(c->algorithm, f.get());
    } catch (const sw::redis::ReplyError& e) {
      // A restarted or promoted node has no scripts: EVAL runs this one and caches it there.
      if (!with_source && std::string_view(e.what()).starts_with("NOSCRIPT")) {
        Send(c, true);
        return;
      }
      if (dynamic_cast<const sw::redis::RedirectionError*>(&e) != nullptr && Reroute(c, with_source, false)) return;
      Fail(*c, e.what(), false, true);
      return;
    } catch (const sw::redis::TimeoutError& e) {
      Fail(*c, e.what(), true);
      return;
    } catch (const std::exception& e) {  // connection lost or refused: the node may have failed over
      if (Reroute(c, with_source, true)) return;
      Fail(*c, e.what());
      return;
    }
    if (r.redis_error) {
      Fail(*c, "unexpected script reply");
      return;
    }
    Health(true, {});
    if (metrics_ != nullptr) metrics_->latency[c->script].Observe(SteadyNowNs() - c->start_ns);
    c->done(r);
  };
  try {
    // AsyncRedisCluster has no routed callback EVALSHA, so a script goes straight to the node the
    // slot map names for its key. After a failover that node is gone or answers MOVED: Reroute.
    auto node = cluster_.redis(c->keys.front(), false);
    if (with_source) {
      node.eval<Reply>(script.source, c->keys.begin(), c->keys.end(), c->args.begin(), c->args.end(),
                       std::move(on_reply));
    } else {
      node.evalsha<Reply>(script.sha, c->keys.begin(), c->keys.end(), c->args.begin(), c->args.end(),
                          std::move(on_reply));
    }
  } catch (const std::exception& e) {  // no usable connection to that node
    if (!Reroute(c, with_source, false)) Fail(*c, e.what());  // never sent
  }
}

// Once per call: a routed command for the key makes redis-plus-plus follow the redirect or the
// failed node and refresh its slot map, and the script is then sent again, to the node now named.
// A redirect or a call never sent did not run; a lost connection may have run it, so a quota
// script is not sent again then (it could count twice, as for ResilientBackend's retries).
bool RedisPPBackend::Reroute(const std::shared_ptr<Call>& c, bool with_source, bool may_have_run) {
  if (c->rerouted || (may_have_run && c->algorithm == Algorithm::kQuota)) return false;
  c->rerouted = true;
  if (metrics_ != nullptr) metrics_->reroutes.Add();
  try {
    cluster_.command<long long>("EXISTS", c->keys.front(), [this, c, with_source](sw::redis::Future<long long>&& f) {
      try {
        f.get();
      } catch (const std::exception&) {  // only the refreshed map matters; the resend reports errors
      }
      Send(c, with_source);
    });
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

void RedisPPBackend::HGetAll(std::string key, HashCallback done) {
  using Fields = std::unordered_map<std::string, std::string>;
  // Shared so `done` survives a call that throws after taking the lambda.
  auto shared_done = std::make_shared<HashCallback>(std::move(done));
  const std::int64_t start = metrics_ != nullptr ? SteadyNowNs() : 0;
  try {
    cluster_.hgetall<Fields>(key, [this, shared_done, start](sw::redis::Future<Fields>&& f) {
      HashResult r;
      try {
        r.fields = f.get();
        Health(true, {});
        if (metrics_ != nullptr) metrics_->latency[5].Observe(SteadyNowNs() - start);  // kCall: hgetall
      } catch (const std::exception& e) {
        Health(false, e.what());
        r.redis_error = true;
        r.timed_out = dynamic_cast<const sw::redis::TimeoutError*>(&e) != nullptr;
        if (metrics_ != nullptr) (r.timed_out ? metrics_->timeouts : metrics_->other_errors).Add();
      }
      (*shared_done)(r);
    });
  } catch (const std::exception& e) {
    Health(false, e.what());
    HashResult r;
    r.redis_error = true;
    (*shared_done)(r);
  }
}

void RedisPPBackend::Fail(Call& c, std::string_view why, bool timed_out, bool reply_error) {
  Health(false, why);
  ScriptResult r;
  r.redis_error = true;
  r.timed_out = timed_out;
  r.reply_error = reply_error;
  if (metrics_ != nullptr) (timed_out ? metrics_->timeouts : reply_error ? metrics_->reply_errors : metrics_->other_errors).Add();
  c.done(r);
}

// Logs when Redis goes from answering to failing and back, never per call.
void RedisPPBackend::Health(bool ok, std::string_view why) {
  if (down_.load(std::memory_order_relaxed) != ok) return;  // no change
  if (down_.exchange(!ok, std::memory_order_relaxed) != ok) return;  // another thread logged it
  if (ok) {
    spdlog::info("redis cluster answering again");
  } else {
    spdlog::error("redis cluster call failed: {}", why);
  }
}

}  // namespace ratelimiter
