#include "usage/usage_log.h"

#include <algorithm>
#include <array>
#include <map>
#include <stdexcept>
#include <utility>

namespace ratelimiter {
namespace {

// Indexed by Reason; kUnknownRule is answered before any engine, so it never becomes usage.
constexpr std::array<const char*, 8> kField{"allowed",     "rate_limited", "quota_exhausted", nullptr,
                                            "unavailable", "unverified",   "overage",         "suspended"};
static_assert(static_cast<std::size_t>(Reason::kSuspended) + 1 == kField.size());

// A JSON string body. Customer IDs come from clients, so anything but printable ASCII is escaped.
std::string Json(std::string_view s) {
  std::string out;
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if (u < 0x20 || u > 0x7e) {
      static constexpr char kHex[] = "0123456789abcdef";
      out += "\\u00";
      out += kHex[u >> 4];
      out += kHex[u & 15];
    } else {
      out += c;
    }
  }
  return out;
}

}  // namespace

UsageLog::UsageLog(const std::string& path, std::string instance, std::size_t capacity,
                   std::chrono::milliseconds every)
    : instance_(Json(instance)),
      capacity_(capacity),
      early_(std::max<std::size_t>(1, capacity / 2)),
      every_(every),
      file_(std::fopen(path.c_str(), "a")) {
  if (!file_) throw std::runtime_error("cannot open usage file " + path);
  buf_.reserve(capacity_);
  spare_.reserve(capacity_);
  thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

UsageLog::~UsageLog() {
  thread_.request_stop();
  thread_.join();
  Flush();
}

void UsageLog::Record(std::string_view customer, Reason reason, std::uint32_t cost) {
  using std::chrono::duration_cast, std::chrono::minutes, std::chrono::system_clock;
  Event e{std::string(customer), duration_cast<minutes>(system_clock::now().time_since_epoch()).count(), reason,
          cost};
  std::lock_guard lock(mu_);
  if (buf_.size() >= capacity_) {
    dropped_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  buf_.push_back(std::move(e));
  if (buf_.size() == early_) wake_.notify_one();
}

void UsageLog::Run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(mu_);
      wake_.wait_for(lock, stop, every_, [this] { return buf_.size() >= early_; });
    }
    Flush();
  }
}

void UsageLog::Flush() {
  {
    std::lock_guard lock(mu_);
    buf_.swap(spare_);
  }
  std::map<std::pair<std::string_view, std::int64_t>, std::array<std::uint64_t, kField.size()>> sums;
  for (const Event& e : spare_) sums[{e.customer, e.minute}][static_cast<std::size_t>(e.reason)] += e.cost;
  for (const auto& [k, n] : sums) {
    std::string line = "{\"instance\":\"" + instance_ + "\",\"customer\":\"" + Json(k.first) +
                       "\",\"minute\":" + std::to_string(k.second);
    for (std::size_t i = 0; i < kField.size(); ++i) {
      if (kField[i]) line += ",\"" + std::string(kField[i]) + "\":" + std::to_string(n[i]);
    }
    line += "}\n";
    std::fputs(line.c_str(), file_.get());
  }
  if (!sums.empty() && std::fflush(file_.get()) != 0) {  // disk full or similar: count what may be lost
    dropped_.fetch_add(spare_.size(), std::memory_order_relaxed);
    std::clearerr(file_.get());
  }
  spare_.clear();
}

}  // namespace ratelimiter
