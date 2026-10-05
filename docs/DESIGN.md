# Design

Status: skeleton. Each section is filled in during the stage noted in its heading.

## 1. Goals and capacity math (Week 2, refined Week 6)

Target: >= 1M `Check`/s across the cluster, p50 < 500 us, p99 < 2 ms (server side).
Fill in: measured per-instance throughput, instances needed, Redis shards needed, assumptions.

## 2. Architecture (Week 3)

ASCII or Mermaid diagram: clients -> gRPC callback server -> lease manager (local, sharded)
-> pipelined Redis batcher -> Redis Cluster (Lua scripts).

## 3. Data model (Week 1)

Keys: `{<key>}:tb` (token bucket hash: `tokens`, `last_refill_ms`) and `{<key>}:sw` (sliding window).
Hash tags keep each key in one slot. `PEXPIRE` expires idle keys. Time source: Redis `TIME` inside the
script, with the reasoning for choosing it over caller timestamps.

## 4. Accuracy bound (Week 3-5)

Worst-case over-admission per refill interval = instances x chunk size. Derivation, tuning guidance,
and the integration test that asserts `admitted <= limit + bound`.

## 5. Trade-offs: lease vs strict (Week 5-6)

Throughput and latency of `strict=true` versus leased mode, with measured numbers.

## 6. Failure modes (Week 4-5)

Redis down, failover, `MOVED`/`ASK`, `NOSCRIPT`, slow Redis, instance crash with leased tokens.

## 7. Tuning guide (Week 6)

Chunk size, flush interval, batch size, thread count, shard count.
