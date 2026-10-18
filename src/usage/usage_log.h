#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "ratelimiter/types.h"

namespace ratelimiter {

// The durable record of what each customer used (src/usage/README.md, steps A13 and B8). Record
// appends to a bounded ring without a lock; a background thread sums the rings per customer and
// UTC minute every second and appends one JSON line per sum to a file:
//   {"instance":"limiter-1","customer":"cust:42","minute":29862720,"allowed":1180,"rate_limited":40,...}
// The numbers are cost units (requests when cost is 1), one field per outcome. A customer busy for
// a whole minute gets a line per flush in it; tools/usage_report.py adds them up.
//
// 16 rings, each thread writing to one of them, so threads rarely share a ring. Each is Vyukov's
// bounded queue: a producer claims a cell with one CAS and publishes it with a release store, and
// the flush thread is the only consumer. Measured before and after: results/2026-10-18_customer_microbench.
class UsageLog {
 public:
  // Appends to `path` (throws std::runtime_error if it cannot be opened). Holds about `capacity`
  // events between flushes (split over the rings), flushing early when a ring is half full; an
  // event beyond a full ring is dropped and counted, so Record never waits for the disk.
  UsageLog(const std::string& path, std::string instance, std::size_t capacity = 65'536,
           std::chrono::milliseconds every = std::chrono::seconds(1));
  ~UsageLog();  // writes what is left

  UsageLog(const UsageLog&) = delete;
  UsageLog& operator=(const UsageLog&) = delete;

  bool Record(std::string_view customer, Reason reason, std::uint32_t cost);  // false: dropped
  // Events lost: buffer full, or the file write failed.
  std::uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  struct Event {
    std::string customer;
    std::int64_t minute{0};
    Reason reason{Reason::kAllowed};
    std::uint32_t cost{0};
  };
  struct Cell {
    std::atomic<std::uint64_t> seq;  // == position: free for it; == position + 1: holds its event
    Event event;
  };
  struct alignas(64) Ring {
    std::unique_ptr<Cell[]> cells;
    std::uint64_t mask{0};
    std::atomic<std::uint64_t> head{0};              // next position to claim (producers)
    alignas(64) std::atomic<std::uint64_t> tail{0};  // next position to read (written by the flush thread)
    bool HalfFull() const {
      return head.load(std::memory_order_relaxed) - tail.load(std::memory_order_relaxed) >= (mask + 1) / 2;
    }
  };
  static constexpr std::size_t kRings = 16;

  void Run(std::stop_token stop);
  void Flush();  // only from the flush thread, or after it stopped

  const std::string instance_;
  const std::chrono::milliseconds every_;
  struct Close {
    void operator()(std::FILE* f) const { std::fclose(f); }
  };
  std::unique_ptr<std::FILE, Close> file_;
  std::array<Ring, kRings> rings_;
  std::mutex mu_;  // only for waking the flush thread early, never per event
  std::condition_variable_any wake_;
  std::atomic<std::uint64_t> dropped_{0};
  std::jthread thread_;  // last: starts after the rest is set up
};

}  // namespace ratelimiter
