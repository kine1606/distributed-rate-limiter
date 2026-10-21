// Week 5 metrics: striped counters, histograms and their Prometheus text, label escaping, and the
// HTTP endpoint over a real socket on 127.0.0.1.
#include "metrics/check_metrics.h"
#include "metrics/exposer.h"
#include "metrics/metrics.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace ratelimiter {
namespace {

TEST(Metrics, ACounterAddsUpEveryThread) {
  Counter c;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&c] {
      for (int i = 0; i < 100'000; ++i) c.Add();
    });
  }
  for (auto& t : threads) t.join();
  EXPECT_EQ(c.Value(), 800'000u);
}

TEST(Metrics, AHistogramRendersCumulativeBucketsSumAndCount) {
  Histogram h;
  h.Observe(30'000);         // 30 µs: the first bucket
  h.Observe(2'000'000);      // 2 ms
  h.Observe(5'000'000'000);  // 5 s: above every bound
  std::string out;
  h.Render(out, "lat_seconds", "rule=\"r\"");
  EXPECT_NE(out.find("lat_seconds_bucket{rule=\"r\",le=\"5e-05\"} 1\n"), std::string::npos) << out;
  EXPECT_NE(out.find("lat_seconds_bucket{rule=\"r\",le=\"0.001\"} 1\n"), std::string::npos);
  EXPECT_NE(out.find("lat_seconds_bucket{rule=\"r\",le=\"0.0025\"} 2\n"), std::string::npos);
  EXPECT_NE(out.find("lat_seconds_bucket{rule=\"r\",le=\"1\"} 2\n"), std::string::npos);
  EXPECT_NE(out.find("lat_seconds_bucket{rule=\"r\",le=\"+Inf\"} 3\n"), std::string::npos);
  EXPECT_NE(out.find("lat_seconds_count{rule=\"r\"} 3\n"), std::string::npos);
  EXPECT_NE(out.find("lat_seconds_sum{rule=\"r\"} 5.00203"), std::string::npos);
}

TEST(Metrics, LabelValuesAreEscaped) { EXPECT_EQ(LabelValue("a\"b\\c\nd"), "a\\\"b\\\\c\\nd"); }

TEST(Metrics, CheckMetricsKnowOnlyConfiguredRulesAndSkipZeroCounts) {
  RuleTable rules{{"burst", Rule{.name = "burst"}}};
  CheckMetrics m(rules);
  ASSERT_NE(m.Of("burst"), nullptr);
  EXPECT_EQ(m.Of("customer-id-as-rule"), nullptr);  // no new series from client input
  m.Of("burst")->by_reason[static_cast<std::size_t>(Reason::kRateLimited)].Add(3);
  std::string out;
  m.Render(out);
  EXPECT_NE(out.find("rl_checks_total{rule=\"burst\",reason=\"rate_limited\"} 3\n"), std::string::npos) << out;
  EXPECT_EQ(out.find("reason=\"ok\"} 0"), std::string::npos);
  EXPECT_NE(out.find("# TYPE rl_checks_total counter\n"), std::string::npos);
}

// One HTTP/1.0 request over a socket; returns the whole answer.
std::string Get(int port, const std::string& path) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<std::uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
    ::close(fd);
    return "connect failed";
  }
  const std::string req = "GET " + path + " HTTP/1.0\r\n\r\n";
  ::send(fd, req.data(), req.size(), 0);
  std::string out;
  char buf[4096];
  for (ssize_t n; (n = ::recv(fd, buf, sizeof buf, 0)) > 0;) out.append(buf, static_cast<std::size_t>(n));
  ::close(fd);
  return out;
}

TEST(MetricsExposer, ServesMetricsAndNothingElse) {
  int scrapes = 0;
  MetricsExposer e("127.0.0.1:0", [&scrapes] {
    ++scrapes;
    return std::string("rl_up 1\n");
  });
  ASSERT_GT(e.port(), 0);
  const auto ok = Get(e.port(), "/metrics");
  EXPECT_EQ(ok.rfind("HTTP/1.0 200 OK\r\n", 0), 0u) << ok;
  EXPECT_NE(ok.find("text/plain; version=0.0.4"), std::string::npos);
  EXPECT_EQ(ok.substr(ok.size() - 8), "rl_up 1\n");
  EXPECT_EQ(Get(e.port(), "/").rfind("HTTP/1.0 404", 0), 0u);
  EXPECT_EQ(scrapes, 1);
}

TEST(MetricsExposer, ABadAddressOrAPortInUseThrows) {
  EXPECT_THROW(MetricsExposer("localhost:9100", [] { return std::string(); }), std::runtime_error);
  MetricsExposer first("127.0.0.1:0", [] { return std::string(); });
  EXPECT_THROW(MetricsExposer("127.0.0.1:" + std::to_string(first.port()), [] { return std::string(); }),
               std::runtime_error);
}

}  // namespace
}  // namespace ratelimiter
