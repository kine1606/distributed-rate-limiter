# Load generator

> Week 2 (ghz), Week 7 if needed (custom client) · Status: `loadgen.cc` done (closed loop, callback API, one TCP connection per channel, log-scale latency histogram); about 2x the rate of ghz per core. Also batched calls (`batch` argument: `CheckBatch`, `checks_per_s`) and `envoyproxy/ratelimit` as a target (rule `envoy:<domain>`, `rls_min.proto`), for `bench/ceiling_compare.sh`. Week 5: open loop done (`rate` argument): calls on a fixed schedule, latency from the scheduled time, the generator's own send lag reported, skips under a cap counted. Freezing the server for 0.5 s at 10,000 calls/s: open loop p99 480.8 ms, closed loop p99 5.8 ms (the closed loop hides the stall)

## What this module does

The load generator plays thousands of gateways sending `Check` calls. You need it because there is
no real traffic in a thesis. It must be faster than the server it tests, or you end up measuring
the load generator instead of the limiter.

## Option 1: ghz (start here)

`ghz` is a ready-made gRPC load tool. It reads the proto, sends calls with templated data, and
reports throughput and latency percentiles as JSON. See the commands in [../README.md](../README.md).

How to know when ghz is the bottleneck: run it against `AllowAllCore`. If ghz uses all of its CPU
while the server still has idle cores, the tool is the limit, and a faster client is needed.

## Option 2: custom C++ client (only if ghz saturates)

```cpp
// loadgen.cc: callback-API client, several channels, open-loop rate
struct Options {
  std::string target = "127.0.0.1:50051";
  int channels = 8;                 // HINT: one channel per core; one channel = one TCP connection
  int in_flight_per_channel = 256;  // calls kept outstanding per channel
  int duration_s = 30;
  std::string key_dist = "uniform"; // uniform | zipf | hot
  bool batch = false;               // use CheckBatch with N entries
};
// HINT: record latency per call into an HdrHistogram-style log-bucket array per thread,
//       merge at the end, print p50/p99/p999 and calls per second as JSON.
// HINT: set a deadline on every call (AGENTS.md rule), and count DEADLINE_EXCEEDED separately.
```

The sim_client and load_client programs from our earlier gRPC walkthrough are a starting point;
ask for them if you want them added here.

## Key distributions

Real traffic is not uniform. A few customers send most requests. Test three shapes: uniform over
many keys (the cardinality test), Zipf with a few hot keys (realistic), and one hot key (worst case
for the lease accuracy bound).

## Done when

- [x] You know, with a number, the maximum rate your load generator can produce: 71,364 calls/s against
      `allow_all` at 12 channels x 16 in flight, where the server (3.3 cores) and the generator share the laptop.
- [x] Every result file records which generator and options produced it: loadgen prints its options in
      its JSON line, ghz keeps them under `options` in its JSON report, and each `summary.txt` starts
      with the script, its settings, the commit and the ghz version.
