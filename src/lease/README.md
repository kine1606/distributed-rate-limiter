# lease/ (Week 3)

Local fast path: sharded, cache-line-aligned map of key -> token lease, served with atomics
only; single-flight refill per key; adaptive chunk size capped at a fraction of the global limit;
refills submitted to the pipelined Redis batcher. Implements `RateLimiterCore`.
