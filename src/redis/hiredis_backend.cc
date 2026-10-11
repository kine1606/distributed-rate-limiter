#include "redis/hiredis_backend.h"

#include <hiredis/hiredis.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <spdlog/spdlog.h>

namespace ratelimiter {
namespace {

constexpr timeval kRedisTimeout{0, 200'000};  // 200 ms for connect and for each command
constexpr auto kReconnectBackoff = std::chrono::milliseconds(500);

std::string ReadFile(const std::string& path) {
  std::ifstream f(path);
  if (!f) throw std::runtime_error("cannot read " + path);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

struct ReplyDeleter {
  void operator()(redisReply* r) const { freeReplyObject(r); }
};
using ReplyPtr = std::unique_ptr<redisReply, ReplyDeleter>;

}  // namespace

std::string BucketKey(std::string_view key, std::string_view rule) {
  return "{" + std::string(key) + "}:" + std::string(rule) + ":tb";
}

void HiredisBackend::ContextDeleter::operator()(redisContext* c) const { redisFree(c); }

HiredisBackend::HiredisBackend(std::string host, int port, std::string lua_path)
    : host_(std::move(host)), port_(port), script_(ReadFile(lua_path)), worker_([this] { Run(); }) {}

HiredisBackend::~HiredisBackend() {
  {
    std::lock_guard lock(mu_);
    stop_ = true;
  }
  cv_.notify_one();
  worker_.join();
}

void HiredisBackend::Eval(const Rule& rule, std::string_view key, std::uint32_t requested,
                          ScriptCallback done) {
  // An idle bucket refills completely after capacity / rate seconds, so expiring it then loses
  // nothing. One extra second of margin covers rounding.
  const double full_after_ms = rule.limit / rule.refill_per_sec * 1000.0;
  Job job{BucketKey(key, rule.name), rule.limit, rule.refill_per_sec, requested,
          static_cast<std::int64_t>(std::ceil(full_after_ms)) + 1000, std::move(done)};
  {
    std::lock_guard lock(mu_);
    if (!stop_) {
      queue_.push_back(std::move(job));
      cv_.notify_one();
      return;
    }
  }
  ScriptResult err;
  err.redis_error = true;
  job.done(err);  // shutting down; never call back while holding mu_
}

void HiredisBackend::Run() {
  for (;;) {
    std::unique_lock lock(mu_);
    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (stop_) {
      std::deque<Job> rest = std::move(queue_);
      lock.unlock();
      ScriptResult err;
      err.redis_error = true;
      for (auto& job : rest) job.done(err);
      return;
    }
    Job job = std::move(queue_.front());
    queue_.pop_front();
    lock.unlock();

    job.done(Execute(job));
  }
}

bool HiredisBackend::EnsureConnected() {
  if (ctx_) return true;

  const auto now = std::chrono::steady_clock::now();
  if (now < next_connect_) return false;  // fail fast instead of a connect per queued call
  next_connect_ = now + kReconnectBackoff;

  ctx_.reset(redisConnectWithTimeout(host_.c_str(), port_, kRedisTimeout));
  if (!ctx_ || ctx_->err) {
    if (!logged_down_) {
      spdlog::error("redis {}:{} unreachable: {}", host_, port_, ctx_ ? ctx_->errstr : "out of memory");
      logged_down_ = true;
    }
    ctx_.reset();
    return false;
  }
  redisSetTimeout(ctx_.get(), kRedisTimeout);

  // Redis returns the script's SHA1; EVALSHA then sends 40 bytes instead of the whole script.
  ReplyPtr r(static_cast<redisReply*>(
      redisCommand(ctx_.get(), "SCRIPT LOAD %b", script_.data(), script_.size())));
  if (!r || r->type != REDIS_REPLY_STRING) {
    spdlog::error("SCRIPT LOAD failed on {}:{}", host_, port_);
    ctx_.reset();
    return false;
  }
  sha_.assign(r->str, r->len);
  spdlog::info("connected to redis {}:{}, token_bucket.lua sha {}", host_, port_, sha_);
  logged_down_ = false;
  return true;
}

ScriptResult HiredisBackend::Execute(const Job& job) {
  ScriptResult out;
  out.redis_error = true;
  if (!EnsureConnected()) return out;

  const std::string capacity = std::to_string(job.capacity);
  const std::string rate = std::to_string(job.refill_per_sec);
  const std::string requested = std::to_string(job.requested);
  const std::string ttl = std::to_string(job.ttl_ms);

  // EVALSHA <sha> 1 <key> capacity rate requested ttl  (see lua/token_bucket.lua)
  auto call = [&](const char* cmd, const std::string& body) {
    std::array<const char*, 8> argv{cmd,           body.c_str(), "1",
                                    job.redis_key.c_str(), capacity.c_str(), rate.c_str(),
                                    requested.c_str(), ttl.c_str()};
    std::array<std::size_t, 8> lens{std::strlen(cmd), body.size(), 1, job.redis_key.size(),
                                    capacity.size(), rate.size(), requested.size(), ttl.size()};
    return ReplyPtr(static_cast<redisReply*>(
        redisCommandArgv(ctx_.get(), static_cast<int>(argv.size()), argv.data(), lens.data())));
  };

  ReplyPtr r = call("EVALSHA", sha_);
  if (r && r->type == REDIS_REPLY_ERROR && std::string_view(r->str, r->len).starts_with("NOSCRIPT")) {
    // Script cache flushed or a fresh node: EVAL runs it and caches it again.
    r = call("EVAL", script_);
  }
  if (!r) {  // I/O error or timeout: the connection is unusable, reconnect on the next call
    spdlog::error("redis {}:{} call failed: {}", host_, port_, ctx_->errstr);
    ctx_.reset();
    return out;
  }
  if (r->type != REDIS_REPLY_ARRAY || r->elements != 3) {
    spdlog::error("token_bucket.lua bad reply: {}",
                  r->type == REDIS_REPLY_ERROR ? std::string(r->str, r->len) : "not a 3-element array");
    return out;
  }
  out.redis_error = false;
  out.allowed = r->element[0]->integer == 1;
  out.remaining = static_cast<std::uint32_t>(r->element[1]->integer);
  out.retry_after_ms = static_cast<std::uint32_t>(r->element[2]->integer);
  return out;
}

}  // namespace ratelimiter
