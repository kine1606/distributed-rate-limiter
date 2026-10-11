# Engines (`RateLimiterCore` implementations)

> Week 2 (StrictCore), Week 3 (LeasedCore), Week 4 (CompositeCore) · Flow steps A6, A11 · Status: `StrictCore` and `LeasedCore` done (`LeasedCore` is the server default and routes `strict: true` rules to `StrictCore`); `CompositeCore` not started

## What this module does

An engine takes a `CheckRequest` and eventually calls `done(CheckResult)`. It is the place where
plans, scripts, leases and policies come together into one decision. The gRPC server only knows
the `RateLimiterCore` interface in `include/ratelimiter/core.h`, so each engine can be tested
without a network and swapped with one line in `main.cc`.

You build three engines, each on top of the previous one:

| Engine | Decides by | Used for | Week |
| --- | --- | --- | --- |
| `StrictCore` | One Redis script call per check | Daily quota; any `strict: true` rule; the baseline | 2 |
| `LeasedCore` | Local lease, Redis only for refills | Burst rules | 3 |
| `CompositeCore` | Quota through Strict, burst through Leased, then overage policy | One API call of a customer | 4 |

## First: extend the domain types

`include/ratelimiter/types.h` must carry what the proto v1 carries, without including any gRPC
header (AGENTS.md, layer decoupling).

```cpp
enum class Reason : std::uint8_t {
  kAllowed, kBurstLimited, kQuotaExhausted, kOverage, kSuspended, kUnavailable
};

struct CheckRequest {
  std::string_view key;         // customer id, e.g. "cust:42"
  std::string_view rule;
  std::uint32_t cost{1};
  std::string_view request_id;  // empty = no deduplication
};

struct CheckResult {
  bool allowed{false};
  Reason reason{Reason::kAllowed};
  std::uint32_t remaining{0};
  std::uint32_t limit{0};
  std::uint32_t retry_after_ms{0};   // burst: when one token is back
  std::uint32_t reset_after_ms{0};   // quota: until 00:00 UTC
  bool overage{false};
};
```

Hint: the `string_view`s point into the protobuf request, which lives until `Finish()`. An engine
that answers later (after Redis) must copy what it needs before the `Check` call returns, or the
views dangle. Copying the key into a `std::string` on the slow path is fine; on the fast path use
the interned key from the lease table instead.

## Week 2: StrictCore

```cpp
class StrictCore final : public RateLimiterCore {
 public:
  StrictCore(RedisBackend& redis, PlanStore& plans, const RuleTable& rules);
  void Check(const CheckRequest& req, CheckCallback done) override;
 private:
  // 1. copy key, cost, request_id (views die when Check returns)
  // 2. plans.Get(customer) -> Plan
  // 3. build ScriptCall{kQuota, keys={"{"+customer+"}:quota"}, args={plan.daily, cost}}
  // 4. redis.Eval(call, [done](const ScriptReply& r){ ...map to CheckResult...; done(result); })
};
```

Build it in this order:

1. Write the key builder once: `QuotaKey("cust:42") -> "{cust:42}:quota"`. Unit-test it. Every
   key in the system must go through these helpers so the hash tag format never drifts.
2. Map the script reply to `CheckResult`. A `kTimeout` or `kUnavailable` reply is not a denial; for
   now return `allowed = false, reason = kUnavailable` and leave real policy to Week 5.
3. Test with `FakeBackend`: 200 allowed then denied; cost greater than limit; unknown customer gets
   Free.
4. Swap `AllowAllCore` for `StrictCore` in `main.cc` and run the Week 2 gate test through gRPC.

## Week 3: LeasedCore

```cpp
class LeasedCore final : public RateLimiterCore {
 public:
  LeasedCore(LeaseTable& leases, RefillCoordinator& refills, PlanCache& plans);
  void Check(const CheckRequest& req, CheckCallback done) override;
  // fast path: leases.TryTake(key, cost) -> done() inline, no allocation, no lock
  // slow path: refills.ParkAndRefill(key, cost, std::move(done))
};
```

The lease details (table, single-flight, chunk sizing) live in [src/lease](../lease/README.md).
This class only chooses fast or slow path and turns the outcome into a `CheckResult`.

Hint: measure `LeasedCore` against `StrictCore` with the same `ghz` command as the Week 2 baseline.
That comparison is the core result of Chapter 5, so keep the command identical.

## Week 4: CompositeCore

One API call of a customer needs two decisions, and the order matters.

```cpp
class CompositeCore final : public RateLimiterCore {
 public:
  CompositeCore(DenyCache& deny, PlanCache& plans, LeasedCore& burst,
                StrictCore& quota, UsageSink& usage);
  void Check(const CheckRequest& req, CheckCallback done) override;
};
```

Build it in this order (this is the decision tree in the flow page, section A2):

1. **Suspended?** Deny with `kSuspended`. No Redis call.
2. **Deny cache hit?** Deny with `kQuotaExhausted` and the cached `reset_after_ms`. No Redis call.
3. **Burst first.** Take from the burst lease. If denied, answer `kBurstLimited` immediately. Burst
   goes first because it is local and cheap, and a request rejected for burst must not consume
   daily quota.
4. **Then quota (strict).** Call the quota script with the plan's overage policy. If denied, give
   the burst tokens back to the lease and write the deny-cache entry.
5. **Map the outcome.** Allowed, allowed with `overage = true` (reason `kOverage`), or denied.
6. **Record usage** with one `UsageSink::Record(...)` call, then `done(result)`.

Hint: the "give the burst token back" step is easy to forget and causes a subtle bug: a customer
over quota slowly drains their own burst bucket and later sees `kBurstLimited` instead of the true
reason. Write a test for it.

Hint: the plan doc also allows the gateway to send quota and burst as two entries of one
`CheckBatch`. `CompositeCore` is the server-side version of the same idea, so the gateway only
sends one `Check`. Which one you expose is your proto v1 decision; implement the one you chose.

## Done when

- [ ] Week 2: `StrictCore` unit tests on `FakeBackend`; gRPC gate test (exactly 200) passes.
- [ ] Week 3: `LeasedCore` unit tests; benchmark vs strict saved in `results/`.
- [ ] Week 4: `CompositeCore` covers every branch of the A2 decision tree with a test.
