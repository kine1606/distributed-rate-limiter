#include "redis/hiredis_backend.h"

#include <hiredis/hiredis.h>

#include <cmath>
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
// Bounds one pipeline: ~1k scripts run in a few ms, far below kRedisTimeout even under overload.
constexpr std::size_t kMaxBatch = 1024;

// Indexed by Algorithm.
constexpr std::array<const char*, 3> kScriptFiles{"token_bucket.lua", "sliding_window.lua", "fixed_window.lua"};
constexpr std::array<const char*, 3> kKeySuffixes{"tb", "sw", "fw"};

std::size_t Index(Algorithm a) { return static_cast<std::size_t>(a); }

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

std::string RedisKey(const Rule& rule, std::string_view key) {
  return "{" + std::string(key) + "}:" + rule.name + ":" + kKeySuffixes[Index(rule.algorithm)];
}

void HiredisBackend::ContextDeleter::operator()(redisContext* c) const { redisFree(c); }

HiredisBackend::HiredisBackend(std::string host, int port, const std::string& lua_dir)
    : host_(std::move(host)), port_(port) {
  for (std::size_t i = 0; i < scripts_.size(); ++i) {
    scripts_[i].file = kScriptFiles[i];
    scripts_[i].source = ReadFile(lua_dir + "/" + kScriptFiles[i]);
  }
  worker_ = std::thread([this] { Run(); });
}

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
  Job job{RedisKey(rule, key), rule.algorithm, {}, std::move(done)};
  switch (rule.algorithm) {
    case Algorithm::kTokenBucket: {
      // An idle bucket refills completely after capacity / rate seconds, so expiring it then
      // loses nothing. One extra second of margin covers rounding.
      const double full_after_ms = rule.limit / rule.refill_per_sec * 1000.0;
      const auto ttl_ms = static_cast<std::int64_t>(std::ceil(full_after_ms)) + 1000;
      job.args = {std::to_string(rule.limit), std::to_string(rule.refill_per_sec),
                  std::to_string(requested), std::to_string(ttl_ms)};
      break;
    }
    case Algorithm::kSlidingWindow:
    case Algorithm::kFixedWindow:  // both scripts compute their own TTL from the window
      job.args = {std::to_string(rule.limit), std::to_string(rule.window_ms), std::to_string(requested)};
      break;
  }
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
    std::deque<Job> batch;
    {
      std::unique_lock lock(mu_);
      cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
      if (stop_ || queue_.size() <= kMaxBatch) {
        batch.swap(queue_);  // take everything queued: one lock per batch, not per call
      } else {
        const auto end = queue_.begin() + static_cast<std::ptrdiff_t>(kMaxBatch);
        batch.assign(std::make_move_iterator(queue_.begin()), std::make_move_iterator(end));
        queue_.erase(queue_.begin(), end);
      }
      if (stop_) {  // fail everything still queued; Eval now answers new calls inline
        lock.unlock();
        ScriptResult err;
        err.redis_error = true;
        for (auto& job : batch) job.done(err);
        return;
      }
    }
    ExecuteBatch(batch);
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

  // Redis returns each script's SHA1; EVALSHA then sends 40 bytes instead of the whole script.
  for (auto& s : scripts_) {
    ReplyPtr r(static_cast<redisReply*>(
        redisCommand(ctx_.get(), "SCRIPT LOAD %b", s.source.data(), s.source.size())));
    if (!r || r->type != REDIS_REPLY_STRING) {
      spdlog::error("SCRIPT LOAD {} failed on {}:{}", s.file, host_, port_);
      ctx_.reset();
      return false;
    }
    s.sha.assign(r->str, r->len);
  }
  spdlog::info("connected to redis {}:{}, {} scripts loaded", host_, port_, scripts_.size());
  logged_down_ = false;
  return true;
}

// Builds "<cmd> <body> 1 <key> <args...>" (EVALSHA <sha> or EVAL <source>) and passes it to `send`.
template <typename Send>
auto WithArgv(const std::string& key, const std::vector<std::string>& args, std::string_view cmd,
              const std::string& body, Send send) {
  std::array<const char*, 8> argv{cmd.data(), body.c_str(), "1", key.c_str()};
  std::array<std::size_t, 8> lens{cmd.size(), body.size(), 1, key.size()};
  std::size_t n = 4;
  for (const auto& a : args) {
    argv[n] = a.c_str();
    lens[n++] = a.size();
  }
  return send(static_cast<int>(n), argv.data(), lens.data());
}

ScriptResult HiredisBackend::Parse(const redisReply* r, const char* file) {
  ScriptResult out;
  if (r->type != REDIS_REPLY_ARRAY || r->elements != 3) {
    spdlog::error("{} bad reply: {}", file,
                  r->type == REDIS_REPLY_ERROR ? std::string(r->str, r->len) : "not a 3-element array");
    out.redis_error = true;
    return out;
  }
  out.allowed = r->element[0]->integer == 1;
  out.remaining = static_cast<std::uint32_t>(r->element[1]->integer);
  out.retry_after_ms = static_cast<std::uint32_t>(r->element[2]->integer);
  return out;
}

void HiredisBackend::ExecuteBatch(std::deque<Job>& batch) {
  ScriptResult err;
  err.redis_error = true;
  if (!EnsureConnected()) {
    for (auto& job : batch) job.done(err);
    return;
  }

  // Pipeline: buffer every call, then read the replies in order. The whole batch costs one round
  // trip, so throughput no longer stops at 1 / RTT.
  for (const auto& job : batch) {
    WithArgv(job.redis_key, job.args, "EVALSHA", scripts_[Index(job.algorithm)].sha,
             [&](int argc, const char** argv, const std::size_t* lens) {
               return redisAppendCommandArgv(ctx_.get(), argc, argv, lens);
             });
  }
  std::vector<Job*> noscript;
  std::size_t i = 0;
  for (; i < batch.size(); ++i) {
    void* raw = nullptr;
    if (redisGetReply(ctx_.get(), &raw) != REDIS_OK) break;
    ReplyPtr r(static_cast<redisReply*>(raw));
    if (r->type == REDIS_REPLY_ERROR && std::string_view(r->str, r->len).starts_with("NOSCRIPT")) {
      noscript.push_back(&batch[i]);  // retried below, once the pipeline is drained
      continue;
    }
    batch[i].done(Parse(r.get(), scripts_[Index(batch[i].algorithm)].file));
  }
  if (i < batch.size()) {  // I/O error or timeout: the connection is unusable, reconnect next batch
    spdlog::error("redis {}:{} call failed: {}", host_, port_, ctx_->errstr);
    ctx_.reset();
    for (; i < batch.size(); ++i) batch[i].done(err);
    for (Job* job : noscript) job->done(err);
    return;
  }

  // Script cache flushed or a fresh node: EVAL runs the script and caches it again. Rare, so these
  // go one at a time.
  for (Job* job : noscript) {
    if (!ctx_) {  // an earlier retry lost the connection; the next batch reconnects
      job->done(err);
      continue;
    }
    const Script& script = scripts_[Index(job->algorithm)];
    ReplyPtr r(static_cast<redisReply*>(
        WithArgv(job->redis_key, job->args, "EVAL", script.source,
                 [&](int argc, const char** argv, const std::size_t* lens) {
                   return redisCommandArgv(ctx_.get(), argc, argv, lens);
                 })));
    if (!r) {
      spdlog::error("redis {}:{} call failed: {}", host_, port_, ctx_->errstr);
      ctx_.reset();
      job->done(err);
      continue;
    }
    job->done(Parse(r.get(), script.file));
  }
}

}  // namespace ratelimiter
