# Evidence: how far one laptop gets, and the limiter next to Envoy's ratelimit service (2026-10-19)

Two questions, measured in one session on one laptop with one load generator:

1. Can this laptop rate limit 200,000 requests per second?
2. How does the limiter compare with an established open-source service of the same kind,
   `envoyproxy/ratelimit` [5], on the same machine and the same load?

Section C then compares the design with other systems built on the same idea, from their own
documentation and source.

## A. The 200,000 question

Medians of 3 runs (range in brackets). "Checks" are rate-limit decisions; a call carries one check,
or a batch of them (`CheckBatch`). Rule `api_default` (token bucket of 1,000, refilled at 500 a
second per key), 10,000 keys unless noted, 192 calls in flight in total.

| Setup | Checks/s | Calls/s | Allowed/s | p50 | p99 | Server CPU | Client CPU |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 instance, one check per call | 75,652 (74,930 to 75,936) | 75,652 | 75,652 | 2.54 ms | 5.79 ms | 3.5 cores | 4.0 cores |
| 4 instances, one check per call | 116,515 (109,641 to 117,671) | 116,515 | 116,519 | 1.51 ms | 4.66 ms | 4.9 cores | 5.2 cores |
| 4 instances of gRPC alone (`allow_all`), control | 108,287 (100,436 to 116,242) | 108,287 | 108,287 | 1.58 ms | 5.09 ms | 4.6 cores | 5.3 cores |
| 1 instance, batches of 10 | **538,605** (521,260 to 552,302) | 53,860 | 538,607 | 3.60 ms | 7.51 ms | 4.0 cores | 3.3 cores |
| 1 instance, batches of 100 | **2,680,850** (2,549,005 to 2,710,477) | 26,808 | 2,680,917 | 7.19 ms | 13.19 ms | 5.1 cores | 2.2 cores |
| 1 instance, batches of 10, **100 keys** (limiting) | 163,830 (157,325 to 164,600) | 16,383 | **49,988** | 11.59 ms | 32.77 ms | 2.8 cores | 1.8 cores |

Answer:

- **200,000 calls of one check each: not on this laptop.** Four instances reach 116,515 checks/s
  and four instances of gRPC alone, with no limiter at all, reach the same (108,287, inside the
  noise). Server and load generator then use about 10 of the 12 hardware threads, so the laptop's
  CPU and gRPC's cost per call are the limit, not the limiter. Reaching 200,000 such calls needs
  more cores, or the load generator on another machine; this laptop cannot show it.
- **200,000 checks a second: yes, with batched calls.** One instance decides 538,605 checks/s in
  batches of 10 and 2,680,850 in batches of 100, because a lease hit costs far less than a gRPC call.
  Batching needs a caller with several checks at once: a request with several limits (Envoy itself
  sends several descriptors per request), or a gateway that groups concurrent requests and pays a
  short wait for it.
- **It still limits correctly at that rate.** With 100 keys each allowed 500 a second, the exact
  limit is 50,000 a second. At 163,830 checks/s the limiter allowed 49,988 a second and denied the
  rest, so it is within 0.03% of the limit. The bucket's initial burst of 1,000 tokens per key was
  spent during the 3 s warm-up. This setup is slower than with 10,000 keys because 1,920 checks in
  flight share 100 leases, and most of them wait for a refill or are denied.

## B. Next to envoyproxy/ratelimit, same laptop and load

Envoy's ratelimit service [5] is a gRPC service in front of Redis: every request updates a Redis
counter (a fixed window per second, minute, hour or day), with an optional local cache of keys
already over their limit. It is the closest open-source counterpart to this limiter's exact path.
Both services ran natively in WSL, both reached Redis through Docker's published ports, and both were
loaded by the same C++ generator (`bench/loadgen`, which speaks both APIs) with 192 calls in flight
over 12 connections. Envoy's limit was 500 a second per customer, so all checks were allowed, as
with `api_default`.

| Service, mode | Redis calls per check | Checks/s | p50 | p99 | Server CPU | Checks/s per server core |
| --- | --- | --- | --- | --- | --- | --- |
| Envoy ratelimit, one descriptor per call | 1 (fixed-window counter) | 38,167 (33,166 to 38,939) | 4.47 ms | 13.78 ms | 5.0 cores | 7,700 |
| This limiter, exact path (`strict`), 12 connections | 1 (token-bucket script) | 10,976 (10,914 to 11,259) | 17.87 ms | 31.38 ms | 2.3 cores | 4,700 |
| This limiter, exact path, 1 connection | 1 (token-bucket script) | 26,964 (24,728 to 27,885) | 6.89 ms | 15.69 ms | 2.6 cores | 10,400 |
| This limiter, leased path | 0.016 (refills; from `throughput_2026-10-19.md`) | **75,652** (74,930 to 75,936) | 2.54 ms | 5.79 ms | 3.5 cores | 21,400 |
| Envoy ratelimit, 10 descriptors per call | 10 | 223,389 (212,723 to 231,945) | 7.51 ms | 27.55 ms | 5.6 cores | 40,000 |
| This limiter, leased, batches of 10 | about 0.016 | **538,605** (521,260 to 552,302) | 3.60 ms | 7.51 ms | 4.0 cores | 133,000 |

With 100 hot keys, Envoy with its over-limit cache on (64 MB) served 48,069 checks/s (47,618 allowed),
and the leased limiter 70,627 (52,582 allowed, the rest denied). At 48,069 checks/s Envoy's 100
customers stayed just under their 500 a second, so its cache had little to do. That pair therefore
shows throughput on hot keys, not the cache.

Readings:

- **The leased path is 2.0 times Envoy's throughput at 0.7 times its CPU** (75,652 against 38,167
  checks/s; 21,400 against 7,700 checks per server core), and 2.4 times with batches. This is the
  lease doing its job: a hit asks no one.
- **The exact path is slower than Envoy's.** At 12 connections it reaches 10,976 checks/s against
  38,167. With one connection it reaches 26,964, using 2.6 cores and with nothing saturated, so the
  cost depends on how many client connections there are, not on Redis. Every Redis reply finishes its
  gRPC call from redis-plus-plus's single I/O thread, so more connections mean more wake-ups of other
  gRPC threads. The leased path finishes calls on the gRPC thread and does not pay this. Envoy's Go
  server finishes each call on the goroutine that waited for Redis. This is the clearest thing the
  comparison teaches: finish exact-path calls on the gRPC threads, or run several Redis I/O threads.
  Three exact-path instances of 64 calls in flight each already reached 53,440 checks/s together in
  an exploratory run.
- **Not like for like, in both directions.** Envoy counts fixed windows with plain Redis commands
  on one Redis node; the exact path runs a token-bucket Lua script on a 3-master Redis Cluster. A
  fixed window is cheaper per call, and it allows a burst of up to twice the limit across a window
  edge, which a token bucket does not.

## C. Same idea, other systems

From each project's documentation or source, read on 2026-10-19 (references in
`docs/thesis/references.md`).

| System | Where a check is decided | Shared state | How it avoids the store on the hot path | Accuracy across instances | Quota and billing |
| --- | --- | --- | --- | --- | --- |
| Envoy ratelimit [5] | the service, per request | Redis counter per request, pool of 10 connections by default | cache of over-limit keys (off by default), write batching (`REDIS_PIPELINE_WINDOW`, off by default) | exact per counter, fixed windows | day and month windows; no overage or retry deduplication |
| Gubernator [20] | the key's owner peer (consistent hashing); with `GLOBAL`, any peer | memory of the peers, no Redis | batching to owners (500 µs window); `GLOBAL` answers locally and sends hits to the owner asynchronously | exact with an owner; `GLOBAL` "allows for greater scale but at the cost of consistency", no bound stated | Gregorian (calendar) durations |
| Bucket4j [21] | the application (a Java library) | Redis, JCache or a database | `batching`; `delaying` serves locally until `maxUnsynchronizedTokens` or a timeout; `predicting` adds a forecast of the cluster rate | delaying can over-consume (its docs say so), by at most each node's unsynchronized tokens | none (a library) |
| Kong rate limiting [3] | the gateway node | memory, datastore or Redis | `sync_rate` above 0: local counters synced periodically | exact only with `sync_rate` 0 | per consumer limits |
| Doorman [9] | the client | Doorman server | client-side capacity leases | cooperative: clients are trusted | none |
| This limiter | the limiter instance | Redis Cluster | server-side leases (one CAS per hit), deny cache, plan cache | burst: at most N × C over any window, proved and measured; quota exact, or limit + N × C for leased large plans | overage policies, request IDs counted once, usage events |

Does the project go the right way? Mostly yes, with one weak spot:

- **The central choice is the established one.** The systems built for scale all decide locally
  and synchronize in amounts: Gubernator's `GLOBAL` mode, Bucket4j's `delaying`, Kong's `sync_rate`,
  and Doorman's leases. The limiter does the same with server-side leases.
- **Two parts go further than these systems.** The over-admission bound N × C is derived and checked
  by a seeded harness and the gate tests; Gubernator and Kong state no bound, and only Bucket4j's
  per-node limit of unsynchronized tokens has the same form. And one call separates an exact quota
  (billed) from a leased burst limit; the other systems apply one accuracy mode to a whole limit.
- **The over-limit cache and store batching are standard.** Envoy has both, Gubernator and Bucket4j
  batch, and so does this design: the deny cache, and the batcher, which was measured and left off
  because it did not help at this scale (Week 3).
- **Weak spot: the exact path.** Envoy's per-request Redis path is 1.4 to 3.5 times faster than
  this limiter's on the same laptop (part B), which matters because quotas of small plans always take
  the exact path. The cause and the fix are known; the fix is Week 7 work.
- **Not compared: production hardening.** Envoy and Gubernator run in production. Metrics, health
  and graceful shutdown here are Week 5.

Published numbers do not allow a comparison: the Envoy README publishes no benchmark, and Gubernator
reports that "a single gubernator node fields over 2,000 requests a second" in Mailgun's production,
which is a load it served, not a measured capacity.

## Setup

- Machine: Intel Core i5-12450H (12 threads), 7 GB RAM given to WSL2 (kernel
  6.6.114.1-microsoft-standard-WSL2), Ubuntu 24.04, Docker 28.5.1. Load generator, servers and Redis
  on this one laptop; power mode and other applications not controlled.
- This limiter: commit `b5ea971` (`build/release/rate_limiter_server`, gcc 13.3, -O3 -march=native),
  `config/rules.yaml`, defaults; Redis: the development cluster (3 masters, Redis 7.4.11), flushed
  before every run.
- Envoy ratelimit: the `/bin/ratelimit` binary of the official image `envoyproxy/ratelimit:f4e76690`
  (commit f4e76690d9ce, 2026-10-06), run natively with default settings (Redis pool of 10, no
  pipeline window, local cache off unless noted, stats off). Redis: a throwaway `redis:7` container,
  flushed before every run.
- Load: `bench/loadgen` (C++, closed loop) with the batch and Envoy options added for this run;
  3 s warm-up, 12 s measured. CPU is sampled from `/proc` over 10 s inside that window (100% = one
  core). Cells ran in turn, three times over.

Limits: closed-loop load hides stalls from the percentiles (open-loop is Week 5); Docker's
network round trip is far shorter than a real network's; client and servers compete for the same
cores, which is what bounds part A.

## How to reproduce

```bash
docker create --name rl-tmp envoyproxy/ratelimit:f4e76690
docker cp rl-tmp:/bin/ratelimit /tmp/envoy-ratelimit && docker rm rl-tmp
docker run -d --name rl-compare-redis -p 127.0.0.1:6390:6379 redis:7
ENVOY_RL=/tmp/envoy-ratelimit RUNS=3 bench/ceiling_compare.sh   # about 11 minutes
```

Raw output: `results/2026-10-19_ceiling/summary.txt` (git-ignored; one JSON line per run). The
script flushes the development cluster and the Redis on port 6390 before every run.
