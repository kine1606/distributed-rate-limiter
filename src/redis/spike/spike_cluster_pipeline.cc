// Week 1 spike, not part of the main build: can redis-plus-plus's async cluster client pipeline
// EVALSHA from several threads over a small connection pool? Prints completed calls per second and
// counts errors by message, so a master can be stopped mid-run to see what Week 5 must handle.
//
// Build against redis-plus-plus built with -DREDIS_PLUS_PLUS_BUILD_ASYNC=libuv:
//   g++ -std=c++20 -O2 src/redis/spike/spike_cluster_pipeline.cc -o build/spike -lredis++ -lhiredis -luv -pthread
// Run from the repo root against the dev cluster (docker/docker-compose.dev.yml):
//   build/spike <threads> <pool size> <seconds> [in flight per thread] [seed port]
#include <sw/redis++/async_redis++.h>
#include <sw/redis++/redis++.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
  const int threads = argc > 1 ? std::stoi(argv[1]) : 4;
  const int pool = argc > 2 ? std::stoi(argv[2]) : 4;
  const int seconds = argc > 3 ? std::stoi(argv[3]) : 5;
  const int window = argc > 4 ? std::stoi(argv[4]) : 1000;
  sw::redis::ConnectionOptions opts;
  opts.host = "127.0.0.1";
  opts.port = argc > 5 ? std::stoi(argv[5]) : 7001;
  opts.connect_timeout = std::chrono::milliseconds(100);
  opts.socket_timeout = std::chrono::milliseconds(100);

  std::stringstream src;
  src << std::ifstream("lua/token_bucket.lua").rdbuf();
  std::string sha;
  sw::redis::RedisCluster(opts).for_each([&](sw::redis::Redis& master) { sha = master.script_load(src.str()); });

  sw::redis::ConnectionPoolOptions pool_opts;
  pool_opts.size = pool;
  sw::redis::AsyncRedisCluster cluster(opts, pool_opts);

  std::atomic<long> ok{0};
  std::atomic<bool> stop{false};
  std::mutex mu;
  std::map<std::string, long> errors;
  std::vector<std::thread> senders;
  for (int t = 0; t < threads; ++t) {
    senders.emplace_back([&, t] {
      // capacity, refill/s, requested, ttl_ms: a bucket that never runs out, so every call is a full write.
      const std::vector<std::string> args{"1000000", "1000000", "1", "60000"};
      std::vector<std::string> keys(window);
      std::vector<sw::redis::Future<std::vector<long long>>> inflight;
      for (long i = t; !stop.load(std::memory_order_relaxed);) {
        inflight.clear();
        for (int w = 0; w < window; ++w, i += threads) {
          keys[w] = "{spike:" + std::to_string(i % 10000) + "}:tb";  // 10k keys over all 3 masters
          inflight.push_back(cluster.evalsha<std::vector<long long>>(sha, keys.begin() + w, keys.begin() + w + 1,
                                                                    args.begin(), args.end()));
        }
        for (auto& f : inflight) {
          try {
            f.get();
            ok.fetch_add(1, std::memory_order_relaxed);
          } catch (const std::exception& e) {
            std::lock_guard lock(mu);
            ++errors[e.what()];
          }
        }
      }
    });
  }

  long last = 0, total_err = 0;
  for (int s = 1; s <= seconds; ++s) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const long now = ok.load();
    long err = 0;
    {
      std::lock_guard lock(mu);
      for (auto& [msg, n] : errors) err += n;
    }
    std::printf("t=%2ds ok/s=%ld errors/s=%ld\n", s, now - last, err - total_err);
    last = now;
    total_err = err;
  }
  stop = true;
  for (auto& t : senders) t.join();
  std::printf("threads=%d pool=%d in_flight/thread=%d: %.0f calls/s\n", threads, pool, window,
              static_cast<double>(last) / seconds);
  for (auto& [msg, n] : errors) std::printf("  %6ld x %s\n", n, msg.c_str());
}
