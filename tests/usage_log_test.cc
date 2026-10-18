// UsageLog (Week 4, steps A13 and B8): sums per customer and minute, escaping, and that every event
// is either written or counted as dropped.
#include "usage/usage_log.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace ratelimiter {
namespace {

std::string TempPath(const std::string& name) {
  const std::string path = ::testing::TempDir() + "usage_" + name + ".jsonl";
  std::remove(path.c_str());
  return path;
}

std::vector<std::string> Lines(const std::string& path) {
  std::ifstream f(path);
  std::vector<std::string> out;
  for (std::string line; std::getline(f, line);) out.push_back(line);
  return out;
}

// Sum of `field` over every line of `customer` (what tools/usage_report.py does).
std::uint64_t Sum(const std::vector<std::string>& lines, const std::string& customer, const std::string& field) {
  std::uint64_t n = 0;
  const std::regex value("\"" + field + "\":([0-9]+)");
  for (const auto& l : lines) {
    std::smatch m;
    if (l.find("\"customer\":\"" + customer + "\"") != std::string::npos && std::regex_search(l, m, value)) {
      n += std::stoull(m[1]);
    }
  }
  return n;
}

TEST(UsageLog, SumsCostPerCustomerAndOutcome) {
  const std::string path = TempPath("sums");
  {
    UsageLog log(path, "limiter-1", 1024, std::chrono::hours(1));  // only the final flush writes
    for (int i = 0; i < 5; ++i) log.Record("cust:1", Reason::kAllowed, 1);
    log.Record("cust:1", Reason::kOverage, 3);
    log.Record("cust:1", Reason::kRateLimited, 1);
    log.Record("cust:2", Reason::kSuspended, 1);
  }
  const auto lines = Lines(path);
  ASSERT_GE(lines.size(), 2u);  // one per customer, or two if a minute ended mid-test
  EXPECT_LE(lines.size(), 4u);
  EXPECT_EQ(Sum(lines, "cust:1", "allowed"), 5u);
  EXPECT_EQ(Sum(lines, "cust:1", "overage"), 3u);  // cost units, not requests
  EXPECT_EQ(Sum(lines, "cust:1", "rate_limited"), 1u);
  EXPECT_EQ(Sum(lines, "cust:1", "suspended"), 0u);
  EXPECT_EQ(Sum(lines, "cust:2", "suspended"), 1u);
  EXPECT_NE(lines[0].find("{\"instance\":\"limiter-1\",\"customer\":"), std::string::npos);
  EXPECT_EQ(lines[0].find("unknown"), std::string::npos);
  EXPECT_EQ(lines[0].back(), '}');
}

TEST(UsageLog, MinuteIsTheUtcMinuteOfTheDecision) {
  const std::string path = TempPath("minute");
  const auto before = std::chrono::duration_cast<std::chrono::minutes>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
  { UsageLog(path, "i").Record("cust:1", Reason::kAllowed, 1); }
  const auto after = std::chrono::duration_cast<std::chrono::minutes>(
                         std::chrono::system_clock::now().time_since_epoch()).count();
  const auto lines = Lines(path);
  ASSERT_EQ(lines.size(), 1u);
  std::smatch m;
  ASSERT_TRUE(std::regex_search(lines[0], m, std::regex("\"minute\":([0-9]+)")));
  EXPECT_GE(std::stoll(m[1]), before);
  EXPECT_LE(std::stoll(m[1]), after);
}

// Customer IDs come from clients: a quote or a newline must not break the JSON line.
TEST(UsageLog, EscapesCustomerIds) {
  const std::string path = TempPath("escape");
  { UsageLog(path, "i").Record("a\"b\\c\nd\xff", Reason::kAllowed, 1); }
  const auto lines = Lines(path);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_NE(lines[0].find("\"customer\":\"a\\\"b\\\\c\\u000ad\\u00ff\""), std::string::npos) << lines[0];
}

TEST(UsageLog, FlushesEverySecondNotOnlyAtShutdown) {
  const std::string path = TempPath("periodic");
  UsageLog log(path, "i", 1024, std::chrono::milliseconds(20));
  log.Record("cust:1", Reason::kAllowed, 1);
  for (int i = 0; i < 200 && Lines(path).empty(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_EQ(Lines(path).size(), 1u);
}

// The response never waits for the disk: a full buffer drops, and every event is either in the
// file or in Dropped(), never lost silently or written twice.
TEST(UsageLog, EveryEventIsWrittenOrCountedAsDropped) {
  const std::string path = TempPath("drops");
  constexpr int kThreads = 4, kEach = 20'000;
  std::uint64_t dropped = 0;
  {
    UsageLog log(path, "i", 64);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&log] {
        for (int i = 0; i < kEach; ++i) log.Record("cust:1", Reason::kAllowed, 1);
      });
    }
    for (auto& t : threads) t.join();
    dropped = log.Dropped();
  }
  EXPECT_EQ(Sum(Lines(path), "cust:1", "allowed") + dropped, std::uint64_t{kThreads} * kEach);
}

TEST(UsageLog, UnopenableFileThrows) {
  EXPECT_THROW(UsageLog("/nonexistent-dir/usage.jsonl", "i"), std::runtime_error);
}

}  // namespace
}  // namespace ratelimiter
