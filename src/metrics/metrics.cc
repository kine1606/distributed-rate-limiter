#include "metrics/metrics.h"

#include <cstdio>

namespace ratelimiter {

void AppendFamily(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
  out.append("# HELP ").append(name).append(" ").append(help).append("\n");
  out.append("# TYPE ").append(name).append(" ").append(type).append("\n");
}

void AppendSample(std::string& out, std::string_view name, std::string_view labels, double value) {
  char number[32];
  std::snprintf(number, sizeof number, "%.17g", value);
  out.append(name);
  if (!labels.empty()) out.append("{").append(labels).append("}");
  out.append(" ").append(number).append("\n");
}

std::string LabelValue(std::string_view v) {
  std::string out;
  for (const char c : v) {
    if (c == '\\' || c == '"') out += '\\';
    if (c == '\n') {
      out += "\\n";
      continue;
    }
    out += c;
  }
  return out;
}

void Histogram::Render(std::string& out, std::string_view name, std::string_view labels) const {
  const std::string bucket = std::string(name) + "_bucket";
  const std::string sep = labels.empty() ? "" : std::string(labels) + ",";
  std::uint64_t cumulative = 0;
  char le[32];
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    cumulative += buckets_[i].Value();
    if (i < kBounds.size()) {
      std::snprintf(le, sizeof le, "%g", kBounds[i]);
    } else {
      std::snprintf(le, sizeof le, "+Inf");
    }
    AppendSample(out, bucket, sep + "le=\"" + le + "\"", static_cast<double>(cumulative));
  }
  AppendSample(out, std::string(name) + "_sum", labels, static_cast<double>(sum_ns_.Value()) / 1e9);
  AppendSample(out, std::string(name) + "_count", labels, static_cast<double>(cumulative));
}

void RedisMetrics::Render(std::string& out) const {
  AppendFamily(out, "rl_redis_latency_seconds", "histogram", "Redis call latency, send to reply, by kind of call");
  for (std::size_t i = 0; i < kCall.size(); ++i) {
    latency[i].Render(out, "rl_redis_latency_seconds", "call=\"" + std::string(kCall[i]) + "\"");
  }
  AppendFamily(out, "rl_redis_errors_total", "counter", "Redis calls that failed, by kind of failure");
  AppendSample(out, "rl_redis_errors_total", "kind=\"timeout\"", static_cast<double>(timeouts.Value()));
  AppendSample(out, "rl_redis_errors_total", "kind=\"reply\"", static_cast<double>(reply_errors.Value()));
  AppendSample(out, "rl_redis_errors_total", "kind=\"other\"", static_cast<double>(other_errors.Value()));
  AppendFamily(out, "rl_redis_reroutes_total", "counter", "Scripts sent again after MOVED or a lost node");
  AppendSample(out, "rl_redis_reroutes_total", "", static_cast<double>(reroutes.Value()));
}

}  // namespace ratelimiter
