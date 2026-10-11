# Design

Status: skeleton with writing hints. Each section names the week it is written in. This file is the
engineering design (what and why, with numbers); the thesis Chapter 3 is written from it, so keep
the two consistent. Replace every `HINT` block with real content and delete the hint.

## 1. Problem and goals (Week 2)

> HINT: Two halves, from the plan. Per customer, a correctness problem: exact daily quotas
> (Free 200, Pay 2,000), retries never double counted, upgrades applied on the next request, one
> reset moment for every instance. Across the platform, a throughput problem: about 1M checks/s,
> p50 < 500 µs and p99 < 2 ms server-side, floods from over-quota clients, hot enterprise keys.
> State the non-goals: billing, accounts, UI, multi-region.

## 2. Architecture (Week 3, final in Week 5)

> HINT: One diagram and one paragraph per box. Reuse the sequence and decision-tree diagrams from
> the Quota Service Flow page (Mermaid renders on GitHub).

```mermaid
flowchart LR
  U[End user app] -->|HTTPS + API key| G[API gateway<br/>gRPC client]
  G -->|Check / CheckBatch| L1[Limiter 1]
  G --> L2[Limiter 2]
  G --> L3[Limiter N]
  subgraph Limiter instance
    S[gRPC callback server] --> C[CompositeCore]
    C --> D[Deny cache]
    C --> LE[Lease table<br/>burst]
    C --> Q[Strict path<br/>quota]
    LE --> B[Batcher]
    Q --> B
  end
  B -->|pipelined EVALSHA| R[(Redis Cluster<br/>3 masters)]
  A[Admin tool rlctl] -->|HSET plan, PUBLISH| R
  G -->|allowed| M[Microservice]
```

## 3. Redis client choice (Week 1)

> HINT: What you tested in the spike (async cluster, pipelining, pool), the measured calls/s for 1,
> 4 and 8 threads, how it behaved when a master died, and the decision. If you fell back to hiredis,
> say what redis-plus-plus lacked.

## 4. Data model (Week 1, extended Week 4)

| Key | Type and fields | Written by | Expires |
| --- | --- | --- | --- |
| `{cust:N}:plan` | HASH plan, daily, burst, refill, overage, extra, failure | `rlctl` | never |
| `{cust:N}:quota` | HASH day, used, overage_used | `quota.lua` | end of day + 1 h |
| `{cust:N}:tb` | HASH tokens (milli), last_refill_ms | `token_bucket.lua` | TTL after last use |
| `{cust:N}:req:<id>` | STRING encoded result | `quota.lua` | 5 min |

> HINT: Explain the hash tag (all keys of a customer in one slot, so one script can use several of
> them), why `used` instead of `remaining` (plan changes never rewrite history), why milli-tokens
> (no float drift), and why Redis `TIME` instead of the caller's clock (one clock for all
> instances; clock skew cannot corrupt state; the cost is that time comes from one node, which is
> acceptable because each key lives on one node anyway).

## 5. Quota and overage logic (Week 4)

> HINT: The three policies with one worked example each: request 2,001 for a `hard`, a `grace` and
> a `cap` customer. Include the decision order from `CompositeCore` (suspended, deny cache, burst,
> quota) and why burst goes first. Explain request ID deduplication and its limit (quota only).

## 6. Lease protocol and accuracy bound (Week 3, measured Week 6)

> HINT: How a lease works (atomic take, single-flight refill, low watermark, adaptive chunk with a
> cap). Then the derivation: with N instances and chunk C, at most N × C tokens are outside Redis,
> so admitted ≤ limit + N × C per refill interval. Then the measured result from the accuracy test
> and a plot of over-admission against chunk size. End with the tuning rule: choose the cap from
> the over-admission you can accept.

## 7. Strict versus leased (Week 3, numbers in Week 7)

| Mode | Redis calls per check | Throughput (1 instance) | p50 | p99 | Accuracy |
| --- | --- | --- | --- | --- | --- |
| Strict | 1 | HINT | HINT | HINT | exact |
| Leased | about 1 / chunk | HINT | HINT | HINT | within N × C |

> HINT: When to choose strict: when one chunk would be a large share of the limit (login attempts,
> Free daily quota) or when exactness is a product promise.

## 8. Failure modes (Week 5)

| Failure | Detection | Behaviour | Recovery |
| --- | --- | --- | --- |
| Redis master down | timeouts, breaker opens | leases serve; then fail open (paid) or closed (free) | replica promoted after node timeout; breaker probe closes |
| Redis slow (20 ms) | latency histogram | HINT | HINT |
| `NOSCRIPT` after failover | error reply | `EVAL` with source once, then `EVALSHA` | automatic |
| `MOVED` / `ASK` | redirect reply | client refreshes slot map and retries | automatic |
| Limiter overloaded | in-flight count | `RESOURCE_EXHAUSTED` | client backs off |
| Limiter crash with leased tokens | | tokens lost (stricter, never looser) | none needed |
| Missed plan-change message | | old plan until cache TTL | TTL expiry |

## 9. Capacity math for 1M checks/s (Week 2 estimate, Week 8 final)

> HINT: Per-instance throughput T (measured), lease hit ratio h (measured), checks per refill c.
> Instances = 1,000,000 / T. Redis calls/s = 1,000,000 × (strict share + (1 − h) for leased
> traffic) / batch factor. Masters needed = Redis calls/s ÷ measured calls/s per master. Write every
> assumption: same per-instance number on server hardware, network RTT, linear scaling as measured
> from 1 to 4 instances.

## 10. Tuning guide (Week 7)

| Knob | Default | Raise it when | Lower it when |
| --- | --- | --- | --- |
| Chunk cap (fraction of limit) | 10% | Redis is the bottleneck | over-admission is too high |
| Batcher `max_delay` | 200 µs | batches are small and Redis is busy | p99 is too high |
| Batcher `max_batch` | 128 | flushes are mostly by size | |
| Plan cache TTL | 5 s | plan reads show in the profile | upgrades must apply faster without Pub/Sub |
| Instances | | CPU of instances is saturated | |
| Redis masters | 3 | a master's CPU is saturated | |

## 11. Limitations and future work (Week 8)

> HINT: Write each limitation the day you meet it. Expected ones: laptop-only measurement, Docker
> network latency far below real networks, burst checks not deduplicated, counters can be lost on a
> failover before replication (usage events are the durable record), single region only.
