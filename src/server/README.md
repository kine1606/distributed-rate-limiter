# gRPC server

> Week 2 (wire StrictCore), Week 4 (proto v1 fields), Week 5 (threads, shutdown, health) · Flow steps A4, A13 · Status: callback server in `service.h` (header-only, run in-process by `tests/grpc_quota_it.cc`); `main.cc` builds `RedisPPBackend` (cluster, default 127.0.0.1:7001), `PlanStore` and the engine chosen by the 5th argument: `leased` (default), `strict`, `allow_all`; proto v1 conversions and request validation in `convert.h`

## What this module does

`main.cc` turns gRPC calls into engine calls and engine results into gRPC replies. It owns the
process: parsing arguments, loading rules, building the engines, starting the server, and shutting
it down cleanly. It must never block a gRPC thread; every slow thing happens in the engine and
completes through a callback that calls `Finish()`.

## What exists today

`RateLimiterService` implements `Check` and `CheckBatch` with the callback API. `Check` validates,
calls the core, and finishes in the callback. `CheckBatch` keeps a shared counter of pending
entries and finishes when it reaches zero. Health and reflection services are registered, and
SIGINT/SIGTERM drain for up to 5 seconds.

## Files to create or change

| File | Change | Week |
| --- | --- | --- |
| `main.cc` | Build `RedisBackend`, `PlanStore`, `StrictCore`; flags for Redis address | 2 |
| `service.h` | done: `RateLimiterService`, header-only so the gRPC test runs it in-process | 2 |
| `convert.h` | done: `Invalid`, `ToDomain`, `Fill`, `ToProto(Reason)`; tested in `tests/convert_test.cc` | 2, 4 |
| `server_options.h` | Thread count, max in-flight, addresses, from flags or env | 5 |

## Skeleton

```cpp
// convert.h: the only place that knows both proto and domain types
inline CheckRequest ToDomain(const v1::CheckRequest& in) {
  return CheckRequest{in.key(), in.rule(), in.cost() == 0 ? 1u : in.cost(), in.request_id()};
}
void Fill(const CheckResult& r, v1::CheckResponse* out);   // maps Reason enum both ways

// service.cc
grpc::ServerUnaryReactor* RateLimiterService::Check(grpc::CallbackServerContext* ctx,
                                                    const v1::CheckRequest* req,
                                                    v1::CheckResponse* resp) {
  auto* reactor = ctx->DefaultReactor();
  // 1. validate -> Finish(INVALID_ARGUMENT) and return
  // 2. admission: if in_flight_ >= max_in_flight_ -> Finish(RESOURCE_EXHAUSTED) and return
  // 3. ++in_flight_; core_->Check(ToDomain(*req), [reactor, resp, this](const CheckResult& r) {
  //        Fill(r, resp); --in_flight_; reactor->Finish(grpc::Status::OK);
  //    });
  return reactor;
}
```

## Build it in this order

1. **Week 2:** (flags skipped: the positional arguments are enough so far) add command-line flags (`--listen`, `--redis`, `--rules`, `--lua-dir`) and build
   `StrictCore` instead of `AllowAllCore`. Keep `AllowAllCore` available behind a flag; it is
   useful to measure pure gRPC overhead in the benchmark.
2. **Week 2:** move the conversions into `convert.h` and unit-test them, especially the `Reason`
   mapping. A wrong mapping turns "come back tomorrow" into "retry in 1 second" at the gateway.
3. **Week 4:** fill every proto v1 field. For `CheckBatch`, check that responses come back in
   request order even when some entries finish on a Redis thread and others inline.
4. **Week 5:** admission control with an `in_flight` counter and `RESOURCE_EXHAUSTED`.
5. **Week 5:** health. Report `NOT_SERVING` while Redis is unreachable (circuit breaker open) and
   during shutdown, so a load balancer stops sending traffic.
6. **Week 5:** shutdown sequence: set health `NOT_SERVING`, stop accepting, wait for `in_flight`
   to reach zero (bounded by a deadline), return leases, flush usage events, then exit.

## Hints

- **`Finish()` exactly once, response untouched after it.** Every path through the handler and the
  callback must end in one `Finish()`. A test that runs each branch of the A2 decision tree and
  counts finishes catches the bugs here.
- **Threads.** gRPC's callback API runs handlers on its own thread pool. You do not pin threads
  yourself until profiling shows it matters; the spec's thread-per-core goal is about not adding
  your own blocking threads, not about rewriting gRPC's executor.
- **Do not log per request.** Count in metrics; log only sampled or rare events.
- **Client cancellation.** If the gateway's deadline passes, gRPC cancels the call. Finishing a
  cancelled call is harmless, but you can skip work by checking `ctx->IsCancelled()` before an
  expensive slow-path step.

## Done when

- [x] Week 2: server runs with `StrictCore` against the Docker cluster; `grpcurl` call works.
- [ ] Week 4: all proto v1 fields filled and tested through a real gRPC client.
- [ ] Week 5: shutdown drains without dropping in-flight calls (test with load running).
