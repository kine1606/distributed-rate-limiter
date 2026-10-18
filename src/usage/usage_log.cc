#include "usage/usage_log.h"

#include <algorithm>
#include <array>
#include <bit>
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
    : instance_(Json(instance)), every_(every), file_(std::fopen(path.c_str(), "a")) {
  if (!file_) throw std::runtime_error("cannot open usage file " + path);
  const std::size_t cells = std::bit_ceil(std::max<std::size_t>(2, capacity / kRings));
  for (Ring& r : rings_) {
    r.cells = std::make_unique<Cell[]>(cells);
    r.mask = cells - 1;
    for (std::size_t i = 0; i < cells; ++i) r.cells[i].seq.store(i, std::memory_order_relaxed);
  }
  thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

UsageLog::~UsageLog() {
  thread_.request_stop();
  thread_.join();
  Flush();
}

bool UsageLog::Record(std::string_view customer, Reason reason, std::uint32_t cost) {
  using std::chrono::duration_cast, std::chrono::minutes, std::chrono::system_clock;
  static std::atomic<std::size_t> next_ring{0};
  thread_local const std::size_t mine = next_ring.fetch_add(1, std::memory_order_relaxed);
  Ring& r = rings_[mine % kRings];
  std::uint64_t pos = r.head.load(std::memory_order_relaxed);
  Cell* c;
  for (;;) {
    c = &r.cells[pos & r.mask];
    const std::uint64_t seq = c->seq.load(std::memory_order_acquire);  // pairs with the flush's release
    if (seq == pos) {
      if (r.head.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) break;
    } else if (seq < pos) {  // the cell still holds an event from one lap ago: the ring is full
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    } else {
      pos = r.head.load(std::memory_order_relaxed);  // another producer took this position
    }
  }
  c->event.customer.assign(customer);
  c->event.minute = duration_cast<minutes>(system_clock::now().time_since_epoch()).count();
  c->event.reason = reason;
  c->event.cost = cost;
  c->seq.store(pos + 1, std::memory_order_release);  // publishes the event to the flush thread
  if (pos + 1 - r.tail.load(std::memory_order_relaxed) == (r.mask + 1) / 2) {  // half full: flush now
    { std::lock_guard lock(mu_); }  // the flush thread is either before its check or waiting: no lost wakeup
    wake_.notify_one();
  }
  return true;
}

void UsageLog::Run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(mu_);
      wake_.wait_for(lock, stop, every_, [this] {
        return std::any_of(rings_.begin(), rings_.end(), [](const Ring& r) { return r.HalfFull(); });
      });
    }
    Flush();
  }
}

void UsageLog::Flush() {
  using Key = std::pair<std::string, std::int64_t>;
  std::map<Key, std::array<std::uint64_t, kField.size()>> sums;
  std::uint64_t events = 0;
  for (Ring& r : rings_) {
    std::uint64_t tail = r.tail.load(std::memory_order_relaxed);
    const Key* last = nullptr;  // a ring mostly repeats one customer and minute: skip the lookup then
    std::array<std::uint64_t, kField.size()>* last_sums = nullptr;
    for (;; ++tail, ++events) {
      Cell& c = r.cells[tail & r.mask];
      if (c.seq.load(std::memory_order_acquire) != tail + 1) break;  // not written yet
      if (last == nullptr || last->second != c.event.minute || last->first != c.event.customer) {
        const auto it = sums.try_emplace(Key{c.event.customer, c.event.minute}).first;  // map nodes never move
        last = &it->first;
        last_sums = &it->second;
      }
      (*last_sums)[static_cast<std::size_t>(c.event.reason)] += c.event.cost;
      c.seq.store(tail + r.mask + 1, std::memory_order_release);  // free for the producer one lap on
    }
    r.tail.store(tail, std::memory_order_relaxed);
  }
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
    dropped_.fetch_add(events, std::memory_order_relaxed);
    std::clearerr(file_.get());
  }
}

}  // namespace ratelimiter
