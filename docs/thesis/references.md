# References

Numbered as cited in the chapters. **V** = content checked online on 2026-10-17/18 while drafting
(the quoted statements were read on the page). **L** = well-known literature cited from memory;
check the bibliographic details against the original before submission. Web pages: accessed
2026-10-17 unless noted.

1. **V** Amazon Web Services. "Usage plans and API keys for REST APIs in API Gateway." *Amazon API
   Gateway Developer Guide.* https://docs.aws.amazon.com/apigateway/latest/developerguide/api-gateway-api-usage-plans.html
   Quoted: "Usage plan throttling and quotas are not hard limits, and are applied on a best-effort
   basis. In some cases, clients can exceed the quotas that you set."
2. **V** Amazon Web Services. "Throttle requests to your REST APIs for better throughput in API
   Gateway." https://docs.aws.amazon.com/apigateway/latest/developerguide/api-gateway-request-throttling.html
   Token bucket throttling; throttles and quotas "should be thought of as targets rather than
   guaranteed request ceilings."
3. **V** Kong Inc. "Rate Limiting" plugin documentation. https://developer.konghq.com/plugins/rate-limiting/
   Policies `local`, `cluster`, `redis`; `redis` is accurate only with `sync_rate` set to 0.
4. **V** Kong Inc. "Rate limiting strategies." https://developer.konghq.com/gateway/rate-limiting/strategies/
5. **V** Envoy Proxy project. *ratelimit* (README). https://github.com/envoyproxy/ratelimit
   Go/gRPC service implementing Envoy's v3 `rls.proto`; Redis (Memcache experimental) as cache;
   per second, minute, hour and day limits; `NEAR_LIMIT_RATIO` default 0.8; optional local cache
   of over-limit keys; shadow mode.
6. **V** Unkey. "Ratelimiting" documentation and engineering notes.
   https://unkey.com/docs/platform/ratelimiting/introduction ,
   https://engineering.unkey.com/architecture/services/api/ratelimiting . Source:
   https://github.com/unkeyed/unkey (AGPL-3.0). Sliding window; regional decisions with global
   convergence within seconds (vendor statement).
7. **V** Upstash. "Ratelimiting algorithms." https://upstash.com/docs/redis/sdks/ratelimit-ts/algorithms
   Fixed window, sliding window and token bucket over (serverless) Redis; fixed window
   recommended for multi-region.
8. **V** Cloudflare. "Counting things, a lot of different things." *The Cloudflare Blog.*
   https://blog.cloudflare.com/counting-things-a-lot-of-different-things/
   Sliding window approximation weighting the previous window; "in practice it proved to be good
   enough."
9. **V** J. Visser, R. Szopa. "Doorman: Global Distributed Client Side Rate Limiting." SREcon16
   Europe, USENIX. https://usenix.org/conference/srecon16europe/program/presentation/visser ;
   code https://github.com/youtube/doorman (archived 2024). Capacity granted as time-limited
   leases with periodic refresh; clients cooperate.
10. **L** C. G. Gray, D. R. Cheriton. "Leases: An Efficient Fault-Tolerant Mechanism for
    Distributed File Cache Consistency." *Proc. 12th ACM Symposium on Operating Systems
    Principles (SOSP)*, 1989.
11. **V** Redis. "Scripting with Lua." https://redis.io/docs/latest/develop/programmability/eval-intro/
    Quoted: "Redis guarantees the script's atomic execution. While executing the script, all server
    activities are blocked during its entire runtime." The script cache "is always volatile";
    `NOSCRIPT`; keys must be passed as key arguments; effects replication is the only mode from
    Redis 7.0, which allows `TIME` before writes.
12. **V** Redis. "Redis cluster specification."
    https://redis.io/docs/latest/operate/oss_and_stack/reference/cluster-spec/
    16384 slots, `HASH_SLOT = CRC16(key) mod 16384`, hash tags, MOVED and ASK, asynchronous
    replication ("small windows where acknowledged writes can be lost").
13. **V** gRPC Authors. "Retry." https://grpc.io/docs/guides/retry/
    Retries only for status codes in the policy's retryable list, with exponential backoff.
14. **L** gRPC Authors. "Deadlines." https://grpc.io/docs/guides/deadlines/ and "Status codes."
    https://grpc.io/docs/guides/status-codes/
15. **L** Google. "Language Guide (proto 3)." *Protocol Buffers Documentation.*
    https://protobuf.dev/programming-guides/proto3/
16. **L** M. Thomson, C. Benfield (eds.). "HTTP/2." RFC 9113, IETF, 2022.
17. **L** R. Fielding, M. Nottingham, J. Reschke (eds.). "HTTP Semantics." RFC 9110, IETF, 2022
    (`Retry-After`).
18. **L** M. Nottingham, R. Fielding. "Additional HTTP Status Codes." RFC 6585, IETF, 2012
    (429 Too Many Requests).
19. **L** J. Heinanen, R. Guerin. "A Single Rate Three Color Marker." RFC 2697, IETF, 1999
    (token buckets with committed and excess burst sizes).
