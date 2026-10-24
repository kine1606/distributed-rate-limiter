# References

Numbered as cited in the chapters. **V** = content checked online on 2026-10-17/18 while drafting
(the quoted statements were read on the page). **L** = well-known literature cited from memory;
check the bibliographic details against the original before submission. **B** = bibliographic
details checked online on 2026-10-25, cited for its standard result. Web pages: accessed 2026-10-17
unless noted.

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
10. **V** C. G. Gray, D. R. Cheriton. "Leases: An Efficient Fault-Tolerant Mechanism for
    Distributed File Cache Consistency." *Proc. 12th ACM Symposium on Operating Systems
    Principles (SOSP '89)*, Litchfield Park, AZ, December 1989, pp. 202-210. ACM, ISBN 0-89791-338-3.
    Checked 2026-10-18 on a scan of the paper (title page numbered 202, "© 1989 ACM
    089791-338-3/89/0012/0202"); pages from dblp. Quoted: "A *lease* is a contract that gives its
    holder specified rights over property for a limited period of time." The abstract: leases are
    "a time-based mechanism"; "Non-Byzantine failures affect performance, not correctness, with
    their effect minimized by short leases."
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
14. **V** gRPC Authors. "Deadlines." https://grpc.io/docs/guides/deadlines/ and "Status Codes."
    https://grpc.io/docs/guides/status-codes/ (accessed 2026-10-18). "By default, gRPC does not set
    a deadline"; past the deadline the call fails with `DEADLINE_EXCEEDED` (4). Codes `OK` (0),
    `INVALID_ARGUMENT` (3), `RESOURCE_EXHAUSTED` (8): "Some resource has been exhausted, perhaps a
    per-user quota, or perhaps the entire file system is out of space", `UNAVAILABLE` (14).
15. **V** Google. "Language Guide (proto 3)." *Protocol Buffers Documentation.*
    https://protobuf.dev/programming-guides/proto3/ (accessed 2026-10-18). A field number "cannot
    be changed once your message type is in use" and "should never be reused"; deleted field numbers
    must be reserved; an unset numeric field reads as zero; the first enum value "must" be zero and
    is best named `..._UNSPECIFIED`.
16. **V** M. Thomson, C. Benfield (eds.). "HTTP/2." RFC 9113, IETF, June 2022 (obsoletes RFC 7540).
    Abstract: "allowing multiple concurrent exchanges on the same connection"; Section 1: "TCP
    head-of-line blocking is not addressed by this protocol."
17. **V** R. Fielding, M. Nottingham, J. Reschke (eds.). "HTTP Semantics." RFC 9110 (STD 97),
    IETF, June 2022. Section 10.2.3: servers send `Retry-After` "to indicate how long the user agent
    ought to wait before making a follow-up request"; the value is an HTTP-date or a number of
    seconds.
18. **V** M. Nottingham, R. Fielding. "Additional HTTP Status Codes." RFC 6585, IETF, April 2012.
    Section 4: "The 429 status code indicates that the user has sent too many requests in a given
    amount of time ("rate limiting")"; the response MAY include `Retry-After`.
19. **V** J. Heinanen, R. Guerin. "A Single Rate Three Color Marker." RFC 2697 (Informational),
    IETF, September 1999. Three parameters, "Committed Information Rate (CIR), Committed Burst Size
    (CBS), and Excess Burst Size (EBS)"; the meter uses "two token buckets, C and E, which both share
    the common rate CIR"
20. **V** Gubernator project (developed at Mailgun). *gubernator* (README and docs/architecture.md).
    https://github.com/gubernator-io/gubernator (Apache-2.0), read 2026-10-19. Peers hold limits in
    memory, "doesn't rely on external caches like memcached or redis"; a consistent hash picks each
    limit's owner; `BATCHING` (default 500 µs window) groups peer requests; `GLOBAL` answers on the
    receiving peer and sends hits to the owner asynchronously: "the use of GLOBAL allows for greater
    scale but at the cost of consistency". Production note: "a single gubernator node fields over
    2,000 requests a second with most batched responses returned in under 1 millisecond".
21. **V** Bucket4j project. *bucket4j*, `io.github.bucket4j.distributed.proxy.optimization`
    (`Optimizations.java`, `DelayParameters.java`). https://github.com/bucket4j/bucket4j (Apache-2.0),
    read 2026-10-19. `batching()` "combines independent requests to same bucket into batches in order
    to reduce request count to remote storage"; `delaying()` "can serve requests locally without
    synchronization with external storage until thresholds are not violated", the thresholds being
    `maxUnsynchronizedTokens` and a maximum time between synchronizations; `predicting()` "tries to
    predict aggregated consumption rate in whole cluster in order to reduce the risk of
    overconsumption that caused by DelayOptimization".
22. **B** J. D. C. Little. "A Proof for the Queuing Formula: L = λW." *Operations Research* 9(3),
    pp. 383–387, 1961. doi:10.1287/opre.9.3.383. The mean number in a system equals the arrival rate
    times the mean time spent in it.
23. **B** N. J. Gunther. *Guerrilla Capacity Planning: A Tactical Approach to Planning for Highly
    Scalable Applications and Services.* Springer, Berlin and Heidelberg, 2007.
    doi:10.1007/978-3-540-31010-5. The Universal Scalability Law, with one coefficient for contention
    and one for coherency delay.
24. **V** B. F. Cooper, A. Silberstein, E. Tam, R. Ramakrishnan, R. Sears. "Benchmarking Cloud
    Serving Systems with YCSB." *Proc. 1st ACM Symposium on Cloud Computing (SoCC '10)*,
    pp. 143–154, 2010. Zipfian requests: "some records will be extremely popular (the head of the
    distribution) while most records will be unpopular (the tail)". The exponent 0.99 is the YCSB
    tool's `ZIPFIAN_CONSTANT` (`core/src/main/java/site/ycsb/generator/ZipfianGenerator.java`,
    read 2026-10-25); the paper does not state it.
25. **B** B. Gregg. "The Flame Graph." *Communications of the ACM* 59(6), pp. 48–57, 2016.
    doi:10.1145/2909476.
26. **V** D. Vyukov. "Intrusive MPSC node-based queue." 1024cores.
    http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
    (the site redirected away on 2026-10-25). gRPC v1.51.1 credits it in
    `src/core/lib/gprpp/mpscq.h`: "Multiple-producer single-consumer lock free queue, based upon the
    implementation from Dmitry Vyukov here", followed by that URL (read 2026-10-25).
