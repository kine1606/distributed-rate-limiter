# Demo gateway (Spring Boot, bonus)

> Week 7, bonus (dropped first if late) · Flow steps A1 to A3, A14 · Status: dropped (2026-10-25): no JDK in
> the build environment; the Python gateway (`examples/gateway-python`, `scripts/demo.sh`) plays this part.

## What this module does

A small Spring Boot app that plays the API owner's gateway for the defense demo. A browser or
`curl` calls `GET /v1/weather` with an API key; the gateway calls the limiter over gRPC and returns
either the weather stub with `200` or `429` with `Retry-After`. It makes the system visible to a
committee in a way a benchmark table cannot, and it plays to your Java/Spring strength.

## Skeleton

```text
examples/gateway-springboot/
  build.gradle                 # grpc-netty-shaded, grpc-protobuf, grpc-stub, protobuf plugin
  src/main/proto/              # symlink or copy of ../../proto/rate_limiter.proto
  src/main/java/.../RateLimitFilter.java
  src/main/java/.../WeatherController.java
```

```java
// RateLimitFilter.java (OncePerRequestFilter)
// 1. read "Authorization: Bearer <key>" -> customer id (a hard-coded Map is enough for a demo)
// 2. stub.withDeadlineAfter(5, MILLISECONDS).check(CheckRequest{key, rule, cost=1, request_id})
// 3. allowed  -> set X-RateLimit-Limit / -Remaining / -Reset, chain.doFilter(...)
// 4. denied   -> 429, Retry-After from retry_after_ms or reset_after_ms (by reason), JSON body
// 5. gRPC error -> fail open for "pay" keys, fail closed for "free" keys (same policy as the server)
```

## Hints

- Create one `ManagedChannel` at startup and reuse it. A channel per request costs a TCP and HTTP/2
  handshake each time.
- For the demo, script three terminals: a free key hitting 429 at request 201, an upgrade with
  `rlctl`, and the same key allowed again on the next request.
- Keep it out of the benchmarks. Its job is the demo, and the JVM's numbers would muddy the
  limiter's.

## Done when

- [ ] Demo script runs end to end in under two minutes and is recorded as a backup video.
