#include "metrics/exposer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ratelimiter {
namespace {

void SendAll(int fd, std::string_view data) {
  while (!data.empty()) {
    const ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
    if (n <= 0) return;  // the scraper went away: nothing to do
    data.remove_prefix(static_cast<std::size_t>(n));
  }
}

}  // namespace

MetricsExposer::MetricsExposer(const std::string& addr, std::function<std::string()> render)
    : render_(std::move(render)) {
  const auto colon = addr.rfind(':');
  sockaddr_in sa{};
  sa.sin_family = AF_INET;
  sa.sin_port = htons(static_cast<std::uint16_t>(colon == std::string::npos ? 0 : std::stoi(addr.substr(colon + 1))));
  if (colon == std::string::npos || ::inet_pton(AF_INET, addr.substr(0, colon).c_str(), &sa.sin_addr) != 1) {
    throw std::runtime_error("metrics address must be ip:port, not '" + addr + "'");
  }
  fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  const int one = 1;
  ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (fd_ < 0 || ::bind(fd_, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0 || ::listen(fd_, 16) != 0) {
    const std::string why = std::strerror(errno);
    if (fd_ >= 0) ::close(fd_);
    throw std::runtime_error("cannot listen for metrics on " + addr + ": " + why);
  }
  socklen_t len = sizeof sa;
  ::getsockname(fd_, reinterpret_cast<sockaddr*>(&sa), &len);
  port_ = ntohs(sa.sin_port);
  thread_ = std::jthread([this](std::stop_token stop) { Run(stop); });
}

MetricsExposer::~MetricsExposer() {
  thread_.request_stop();
  thread_.join();
  ::close(fd_);
}

void MetricsExposer::Run(std::stop_token stop) {
  while (!stop.stop_requested()) {
    pollfd p{fd_, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) continue;  // wakes every 200 ms to see `stop`
    const int c = ::accept4(fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) continue;
    const timeval limit{1, 0};  // a scraper that sends nothing must not hold the thread
    ::setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof limit);
    char buf[2048];
    const ssize_t n = ::recv(c, buf, sizeof buf, 0);
    const std::string_view request(buf, n > 0 ? static_cast<std::size_t>(n) : 0);
    if (request.starts_with("GET /metrics ") || request.starts_with("GET /metrics?")) {
      const std::string body = render_();
      SendAll(c, "HTTP/1.0 200 OK\r\nContent-Type: text/plain; version=0.0.4\r\nContent-Length: " +
                     std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n");
      SendAll(c, body);
    } else {
      SendAll(c, "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
    }
    ::close(c);
  }
}

}  // namespace ratelimiter
