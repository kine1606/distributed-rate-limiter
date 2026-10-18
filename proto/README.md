# API contract (`rate_limiter.proto`)

> Week 1 (design), Week 4 (CheckBatch for quota plus burst) · Flow steps A3, A13 · Status: v1 reviewed and tagged `proto-v1`

## What this module does

The proto file is the contract between the gateway (gRPC client) and the limiter (gRPC server).
Everything the gateway needs to turn a decision into an HTTP response must be in `CheckResponse`,
and everything the limiter needs to decide must be in `CheckRequest`. Changing field numbers later
breaks every client, so this is the one file worth getting right in Week 1.

## Questions to answer before you write v1

Answer each in one or two sentences inside the proto as comments. Your answers become part of
Chapter 3.

1. **Who is limited?** Is `key` a raw customer ID (`cust:42`), or does the limiter build keys from a
   `customer_id` plus the rule? Building keys inside the limiter keeps the hash tag format
   (`{cust:42}:quota`) in one place, which is safer than trusting every gateway to format it.
2. **One check or two?** An API call needs both the daily quota and the burst limit. Either the
   gateway sends a `CheckBatch` with two entries, or a single `Check` names a plan and the limiter
   applies both rules. The plan doc assumes the first; the second is simpler for gateways. Pick one
   and write why.
3. **Why was it denied?** The gateway must tell "slow down for 1 second" apart from "come back
   tomorrow" and "your account is suspended". That is the `reason` enum.
4. **When can it retry?** `retry_after_ms` (burst, short) and `reset_after_ms` (quota, until
   00:00 UTC) mean different things. Keep both.
5. **Was this a retry?** `request_id` lets the limiter ignore a duplicate. Decide who generates it
   (the gateway) and how long it is remembered (a few minutes).
6. **What does an error mean?** A denial is `OK` with `allowed = false`. Status codes are only for
   requests the limiter cannot answer. List which codes you return.

## Skeleton to fill in

This is a shape, not the answer. Replace every `HINT` with your decision and delete the hint.

```proto
syntax = "proto3";
package ratelimiter.v1;

service RateLimiter {
  rpc Check(CheckRequest) returns (CheckResponse);
  // HINT: keep CheckBatch. Week 4 uses it for quota + burst in one RPC.
  rpc CheckBatch(CheckBatchRequest) returns (CheckBatchResponse);
}

message CheckRequest {
  string key = 1;          // HINT: customer id only, or the full key? (question 1)
  string rule = 2;         // HINT: rule name from config/rules.yaml
  uint32 cost = 3;         // HINT: 0 means 1? or reject 0 with INVALID_ARGUMENT?
  string request_id = 4;   // HINT: empty = no deduplication
}

enum Reason {
  REASON_UNSPECIFIED = 0;  // proto3 rule: the zero value must exist and mean "not set"
  // HINT: OK, BURST_LIMITED, QUOTA_EXHAUSTED, OVERAGE, SUSPENDED, UNAVAILABLE ...
}

message CheckResponse {
  bool allowed = 1;
  uint32 remaining = 2;
  uint32 retry_after_ms = 3;
  uint32 limit = 4;
  // HINT: reset_after_ms, reason, maybe overage flag or overage_used
}

message CheckBatchRequest  { repeated CheckRequest requests = 1; }
message CheckBatchResponse { repeated CheckResponse responses = 1; }  // same order as requests
```

## Hints

- **Never renumber or reuse a field number.** Add new fields with new numbers. If you remove one,
  write `reserved 5;` so nobody reuses it by accident. This is the rule that lets old gateways keep
  working while you change the server.
- **Enums start at 0 = UNSPECIFIED.** A proto3 field that was never set reads as 0. If 0 meant
  `OK`, a bug that forgets to set `reason` would look like a success.
- **Keep the response small.** It is encoded once per check; at 100,000 checks/s every byte costs.
  No strings in the response except, maybe, a debug message you leave empty in production.
- **`uint32` is enough** for milliseconds until reset (at most 86,400,000) and for counts.
- **Document semantics in comments,** not only types. "remaining: best effort when served from a
  lease" is information the gateway developer needs.
- After changing the proto, `ghz` and the load generator need the same file. Keep one copy only.

## Done when

- [x] `protoc` compiles it (the CMake build passes).
- [x] Every question above has a written answer in a comment.
- [x] `src/server/main.cc` fills every new response field (even if with a placeholder value until
      the engine exists). Conversions live in `src/server/convert.h`, tested in `tests/convert_test.cc`.
- [x] Reviewed; tagged `proto-v1` (2026-10-18).
