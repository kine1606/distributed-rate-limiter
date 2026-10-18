#pragma once

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
#include <vector>

#include "ratelimiter/types.h"

namespace ratelimiter {

// The durable record of what each customer used (src/usage/README.md, steps A13 and B8). Record
// appends to a bounded buffer; a background thread sums it per customer and UTC minute every
// second and appends one JSON line per sum to a file:
//   {"instance":"limiter-1","customer":"cust:42","minute":29862720,"allowed":1180,"rate_limited":40,...}
// The numbers are cost units (requests when cost is 1), one field per outcome. A customer busy for
// a whole minute gets a line per flush in it; tools/usage_report.py adds them up.
//
// shortcut: one mutex for the whole buffer, taken once per customer decision. Measure under load
// before replacing it with per-thread buffers.
class UsageLog {
 public:
  // Appends to `path` (throws std::runtime_error if it cannot be opened). Holds at most `capacity`
  // events between flushes, flushing early at half; an event beyond that is dropped and counted,
  // so Record never waits for the disk.
  UsageLog(const std::string& path, std::string instance, std::size_t capacity = 65'536,
           std::chrono::milliseconds every = std::chrono::seconds(1));
  ~UsageLog();  // writes what is left

  UsageLog(const UsageLog&) = delete;
  UsageLog& operator=(const UsageLog&) = delete;

  void Record(std::string_view customer, Reason reason, std::uint32_t cost);
  // Events lost: buffer full, or the file write failed.
  std::uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  struct Event {
    std::string customer;
    std::int64_t minute;
    Reason reason;
    std::uint32_t cost;
  };
  void Run(std::stop_token stop);
  void Flush();  // only from the flush thread, or after it stopped

  const std::string instance_;
  const std::size_t capacity_;
  const std::size_t early_;  // flush before the period ends once this many events wait
  const std::chrono::milliseconds every_;
  struct Close {
    void operator()(std::FILE* f) const { std::fclose(f); }
  };
  std::unique_ptr<std::FILE, Close> file_;
  std::mutex mu_;
  std::condition_variable_any wake_;
  std::vector<Event> buf_;    // guarded by mu_
  std::vector<Event> spare_;  // the flush thread's; swapped with buf_, so neither reallocates
  std::atomic<std::uint64_t> dropped_{0};
  std::jthread thread_;  // last: starts after the rest is set up
};

}  // namespace ratelimiter
