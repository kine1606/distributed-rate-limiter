# Chapter 3. Design

> Started Week 2 (2026-10-18) with the product decisions (3.1.3); the lease protocol and its bound
> (3.5) added in Week 3; quota and overage logic (3.4) in Week 4. The rest of the chapter (3.1 to 3.8 in `OUTLINE.md`) is written in Week 5
> from `docs/DESIGN.md`. Section numbers may shift then. Results still to come are marked
> **[Pending: Week N]**.

## 3.1 Requirements

### 3.1.3 Product decisions

Some requirements are not technical consequences but product choices: several answers would be
defensible, and the system must pick one and state why. Table 3.x lists the six that affect how
quotas are counted. Each is enforced in one place, so changing a decision later changes one
component, not the whole design.

*Table 3.x: Product decisions.*

| Question | Decision | Rejected alternative |
| --- | --- | --- |
| When does the day reset? | 00:00 UTC for every customer | midnight in the customer's time zone |
| Does a call that fails with a server error count? | yes | refund the unit on a 5xx response |
| When does a downgrade apply? | at the end of the billing cycle | immediately |
| Is traffic admitted while Redis is unreachable billed? | no: it is marked unverified | billed like counted traffic |
| What if a customer has no plan hash? | treat the customer as Free | deny |
| How large is the grace allowance? | 10% of the daily quota, stored per plan | a fixed amount for every plan |

**Reset at 00:00 UTC.** The quota script derives the current day from the Redis clock as
⌊now / 86,400,000 ms⌋. Every limiter instance therefore agrees on the reset instant (goal G4), the
script needs no time-zone table, and no job runs at midnight: the first call of a new day sees a new
day number and starts from zero. Per-customer midnight would require a time zone in every plan,
would make "today" differ between customers, and would give some customers 23- and 25-hour days at
daylight-saving transitions. It is listed as future work.

**Server errors count.** The limiter decides before the call reaches the provider's service and
never sees its outcome. Refunding a unit after a 5xx response would need a second message from the
gateway after the response, adding a call per failed request and a new failure mode: a lost refund
message silently overcharges, a duplicated one undercharges. The call also did consume the
provider's capacity. A refund keyed by `request_id` is possible future work, which the API already
supports because every call can carry an id.

**Downgrades at the end of the cycle.** A customer has paid for the current cycle, and the quota
script reads the limit from the plan at the time of each call, so a downgrade is simply written to
the plan hash later, by the admin tool, when the cycle ends. If a downgrade does apply on a day
whose usage already exceeds the new limit, the script denies with `remaining = 0`, never a negative
number (`QuotaDowngradeBelowUsageDeniesWithoutNegativeRemaining`). An immediate downgrade would cut
service the customer has already paid for.

**Fail-open traffic is not billed.** When Redis cannot be asked and a plan fails open, the call is
admitted without being counted, and the response says so (`REASON_UNVERIFIED`). Billing such calls
would bill traffic that was never measured and might exceed the quota; the exactness promise of this
thesis covers counted traffic only. Usage events (Week 4) mark these calls `unverified`, so a billing
system can apply its own policy to them.

**A missing plan means Free.** A plan hash can be missing because a sign-up has not propagated yet,
or because of an operator error. Treating the customer as Free can give away at most the cheapest
plan, while denying would turn a data error into an outage and fail every new customer whose first
call races the sign-up. The same rule applies to individual fields: a missing or malformed field
takes its Free default (`plan_test.cc`).

**Grace of 10% of the quota.** A single rule is easy to state in a price list and to test: a Pay
customer with 2,000 calls per day may make 2,200 before being denied (the grace policy is built in
Week 4). The amount is stored per plan
in the `extra` field of the plan hash, written by the seed tool and the admin tool, so a plan can
carry a different allowance without a code change; 10% is the value of the shipped presets.

## 3.4 Quota and overage logic

### 3.4.1 One rule per API call

A gateway asks one question per API call: `Check{key: "cust:42", rule: "customer"}`. The `customer`
rule names two others, a token bucket for the burst limit and a quota for the daily limit, and the
engine reads the customer's plan once and decides in a fixed order:

1. **Suspended plan:** deny with `SUSPENDED`. No script runs, and waiting does not help.
2. **Local denial:** if this instance recently learned that the customer is out of quota, deny with
   `QUOTA_EXHAUSTED` and the reset time it learned. No script runs (3.4.4).
3. **Burst:** take a token from the burst bucket. A denial answers `RATE_LIMITED` at once.
4. **Daily quota:** count the call against the day under the plan's overage policy (3.4.2). If the
   quota denies, the burst token taken in step 3 is given back.

The order follows from what each answer costs and means. A suspended account and a known denial cost
no Redis call, so they come first. Burst comes before the quota because a call refused for burst must
not consume daily quota: the customer did not get the service. The give-back in step 4 is the
mirror case. Without it, a customer over quota would drain their own burst bucket with denied calls
and later be told `RATE_LIMITED` instead of the true reason, `QUOTA_EXHAUSTED`, which tells the
client to wait for the reset rather than retry in a second.

The quota script stores what was used, not what remains, and receives the limit from the current plan
on every call (3.3). A plan change therefore rewrites no counter: an upgrade from 200 to 2,000 a day
turns 200 used into 1,800 remaining on the next call, and a downgrade below today's usage denies with
`remaining = 0`, never a negative number.

### 3.4.2 Overage policies

What happens at the limit is a product choice, so the plan carries it as a policy and an amount
(`overage` and `extra` in the plan hash):

- **hard** denies at the limit until 00:00 UTC.
- **grace** allows `extra` more units free of charge, then denies. The shipped Pay plan has a grace of
  10% (3.1.3).
- **cap** allows up to `extra` units of billed overage and marks each such call `OVERAGE`. The script
  counts overage in a second field, `overage_used`, so the plan's own counter never passes the limit
  and billing reads the overage directly. A call that straddles the limit (cost 2 with one unit
  left) bills only the part beyond it.

Request 2,001 of a plan of 2,000 a day shows the difference. Table 3.y gives the answer of the same
call on three customers whose plans differ only in the policy.

*Table 3.y: Request 2,001 on three overage policies (2,000 used, plan of 2,000 a day).*

| Policy | Shipped plan | Answer to request 2,001 | State after it |
| --- | --- | --- | --- |
| hard | Free | denied, `QUOTA_EXHAUSTED`, retry at 00:00 UTC | used 2,000 |
| grace, extra 200 | Pay | allowed, `OK`, remaining 0 | used 2,001; denied after 2,200 |
| cap, extra 1,000 | Pay with cap | allowed, `OVERAGE` | used 2,000, overage_used 1; denied after 1,000 more |

Grace answers `OK`, not `OVERAGE`, because the extra units are free: the reason field tells the
gateway what to bill, and only cap overage is billed. That choice is also what gives request 2,001
three different answers, which is the Week 4 gate. It is tested on the script alone and end to end
through gRPC (`Request2001GivesThreeAnswersOnThreePolicies`). Both counters reset with the UTC day,
derived from the Redis clock so every instance agrees on the reset instant.

### 3.4.3 Retries counted once

A gateway that times out and retries must not charge the customer twice, and must give the client
the same answer as the first attempt. A call may carry a `request_id`. The quota script then stores
its answer under a key with the customer's hash tag, `{cust:42}:daily_quota:req:<id>`, for five
minutes, in the same atomic script call that counts the usage. A retry with the same id gets the
stored answer, denials included, and is not counted again. The answer is marked `replayed`, so the
retry produces no second usage event and a replayed denial, decided under the plan of the first
attempt, is not cached locally under a plan that may have changed since. Storing the answer rather
than a "seen" flag matters: a retry of a denied call must be denied too, even if the customer was
upgraded in between.

Two limits remain. Burst is not deduplicated: a retry is still traffic, and it takes a burst token.
And the local denial of 3.4.4 answers before Redis, so a retry of a call that was allowed, arriving
after the quota ran out on that instance, is denied. It is not counted twice, but its answer differs
from the original.

### 3.4.4 Local denials and plan changes

After a `QUOTA_EXHAUSTED` answer the instance remembers the customer until the reset time Redis
returned, so further calls are denied without a Redis call and the local entry ends exactly when the
Redis counter does. Each entry keeps a stamp of the plan's quota terms (daily limit, policy, extra).
A call under a different plan ignores and drops the entry, so an upgrade lifts the denial on the next
call even if no plan-change message arrives.

Plans themselves are cached on each instance for five seconds, which removed the plan read from
every call: on the Week 2 strict benchmark the cache alone raised throughput from 16,738 to 19,480
checks per second (median of five runs each, same session). The admin tool writes a plan, then
publishes the customer id on a Pub/Sub channel, and every instance drops its cached plan and its local
denial. A read in flight when the message arrives is discarded and sent again, because it may have
read the old plan. An instance that loses its subscription drops every cached plan when it
reconnects, because Pub/Sub keeps no messages for an absent subscriber. A plan change therefore
applies within the delay of the message, measured at 22 ms from starting the admin tool until two
instances held the new plan, and within the five-second TTL if a message is lost while the
connection looks healthy.

### 3.4.5 Leased quota for large plans

An exact quota costs one Redis call per check, which is the right price for small plans: one chunk
of a 200-a-day quota would be a visible share of the day. For a large plan it is not. A plan of
100,000 calls a day or more therefore takes its daily quota from a lease, with the protocol of 3.5
and a smaller chunk: at most 0.1% of the daily limit, so C = 1,000 for the enterprise plan of
1,000,000. The lease is refilled by the quota script itself, with the chunk as the cost and the hard
policy, so a lease only ever takes units inside the plan. When the day's counter has less than a
chunk left, the lease takes what is left and then denies locally until the reset; a call it denies
goes to the exact path, which applies the plan's overage policy and request id. Overage, which is
billed, is therefore always counted exactly.

The bound of 3.5.3 applies with no refill during the day. Redis grants at most `limit` units in a day.
Units leased before midnight and used after it were counted for the old day, so N instances admit at
most limit + N × C in one day: 0.4% of the day for four instances. Units leased and not used before
the reset are an under-admission of at most N × C. In the gate test four instances shared one
customer of 100,000 a day (C = 100) with 20% more demand than the bound allows. They admitted exactly
100,000 units, with 1,253 quota calls for 12,048 checks instead of one per check. A leased call does
not deduplicate request ids: a retry inside the plan counts again and uses up one more unit of the
plan, though the retry itself is never billed as overage.

The accuracy of leased quota under faults and over a day boundary is evaluation test 4
**[Pending: Week 6]**.

## 3.5 Lease protocol and the accuracy bound

### 3.5.1 Why leases

A strict check costs one Redis round trip, and its throughput on one instance is bounded by how many
round trips are in flight: the Week 2 baseline admits 10,024 checks per second at a p99 of 10.5 ms
with 64 concurrent calls, and needs 1,024 concurrent calls to reach 25,301. The burst limit is
checked on every API call, almost always for customers far below their limit, so most of those round
trips only confirm that tokens remain. A lease removes them: each limiter instance takes a share of a
customer's token bucket from Redis in advance and admits checks against it in its own memory, asking
Redis again only when the share runs low. The cost is accuracy: tokens one instance holds are
invisible to the others. This section defines the protocol, states the invariant it maintains, and
derives the bound on the over-admission that invariant allows.

### 3.5.2 The protocol

A *lease* is the set of tokens of one (rule, key) bucket that one instance has already taken from
Redis. Redis remains the owner of the bucket: it refills the bucket over time with `token_bucket.lua`
and grants tokens to leases on request, atomically.

1. **Hit.** A check of cost c takes c tokens from the lease with one compare-and-swap that never lets
   the count fall below zero. It takes no lock, allocates no memory and sends nothing to Redis.
2. **Single-flight refill.** At most one refill per lease is in flight. A check that finds the lease
   empty is parked; when the refill lands, the parked checks are answered in arrival order. Five
   hundred concurrent misses on one key cause exactly one refill (tested).
3. **Early refill.** A hit that leaves less than 30% of the current chunk starts a refill, so a key
   under steady load never waits for Redis.
4. **Top-up.** A refill asks Redis for `target − tokens held`, where the target is the chunk (or the
   cost of a parked check, if larger). After it lands, the lease holds at most one chunk.
5. **Large checks.** A check whose cost exceeds the chunk cap C skips the lease and is checked in
   Redis directly, on the same bucket.
6. **Adaptive chunk.** The chunk starts at the cap C = max(1, ⌊limit / 10⌋) and changes once per full
   refill: it doubles when checks found the lease dry while the refill was out, because demand
   outran one chunk within one round trip, and halves when the previous chunk lasted more than ten
   seconds, because its tokens sat unused where no other instance could use them. It never leaves
   [min(8, C), C].
7. **Denial.** When Redis reports the bucket empty, the lease denies locally until the retry time
   Redis returned, so a client hammering an empty bucket costs no further Redis calls.

### 3.5.3 Invariant and bound

**Invariant.** At any moment an instance holds at most C tokens of one bucket, apart from the instant
between a refill landing and the parked check that asked for those tokens taking them.

Steps 4 and 5 exist to keep this invariant; 3.5.4 explains how its absence was found.

**Theorem.** Let N instances serve one bucket of capacity `limit` and refill rate r, each holding at
most C tokens. Over any interval [a, b], the tokens admitted satisfy

    admitted(a, b) ≤ limit + r·(b − a) + N·C.

*Proof.* Every token admitted during [a, b] was either held by some instance at time a, or granted
by Redis during [a, b]: leases only hand out tokens that Redis granted. By the invariant, the
instances hold at most N·C tokens at a. Redis executes each grant atomically against the bucket,
which contains at most `limit` tokens at a and gains r·(b − a) during the interval, so it grants at
most limit + r·(b − a). Adding the two bounds gives the result. ∎

An exact token bucket admits at most limit + r·(b − a) over the same interval, so the over-admission
caused by leasing is at most **N × C in any window**. It is temporal, not cumulative: tokens taken
from Redis earlier are spent later, but no token is ever created, so over a whole day the leases
admit no more than Redis granted. The tuning rule follows directly: the cap is chosen from the
over-admission one can accept. With four instances and the default 10% cap, a burst limit of 1,000
may admit up to 400 extra checks in a window, and a cap of 1% would reduce that to 40 at the cost of
ten times more refills.

### 3.5.4 Evidence: a seeded harness

The theorem holds only if the implementation keeps the invariant, which is hard to see in concurrent
code. A deterministic harness checks it. One to four engine instances share a fake Redis that executes
the arithmetic of `token_bucket.lua` (milli-tokens, a millisecond clock, refill on use) at the
moment it answers a call. It answers pending calls in an order drawn from a seeded random generator,
fails 2% of them, and runs under a fake clock that advances by milliseconds with occasional jumps of
up to three seconds. Each seed draws a limit between 10 and 2,000, a refill rate, and checks of cost 1
to 5 on up to three keys, then verifies the theorem over every window of admissions in one pass
(prefix sums of admitted cost, with a running minimum). A failing seed prints itself and replays
exactly.

The harness found two violations in the first version of the protocol:

1. The early refill added a full chunk on top of the up to 30% still in the lease, so an instance
   could hold 1.3 × C. Seed 1177 (N = 2, C = 9) admitted 18.79 tokens over the exact bucket in one
   window, against a bound of 18. The top-up rule (step 4) fixed it.
2. A check costing more than C made the lease hold more than C, through refills sized for it and
   through partial grants it could not use. Seed 4388 (N = 2, C = 1, costs up to 5) admitted 2.7 over
   the exact bucket against a bound of 2. Routing such checks to Redis (step 5) fixed it.

With both fixes, 100,000 seeds (165 million checks) keep the bound, and the worst window reaches
exactly N × C: the bound is tight, not merely safe. The measured over-admission on the real cluster
as a function of chunk size, and its comparison with a simulation, is evaluation test 4
**[Pending: Week 6]**.

### 3.5.5 Alternatives considered

- **No lease (strict checks only).** Exact, but one Redis round trip per check: rejected for the
  burst limit by the throughput argument of 3.5.1, kept for the daily quota where exactness is the
  product.
- **Adding a full chunk at the watermark.** Simpler, and up to 30% fewer refills than the top-up, but
  it breaks the invariant (3.5.4). The top-up costs refills of 0.7 to 1.0 × C instead of exactly C;
  how many more refills that means under load is measured in Week 7 **[Pending]**.
- **`fetch_sub` instead of a compare-and-swap loop.** Measured 2.5 to 2.7 times faster on one shared
  lease under contention, but a failed take must be undone, and while it is undone another thread
  can be denied although tokens exist. The compare-and-swap never lets the count go negative.
  Rejected unless a profile of the real server shows the take on its critical path.
- **Cooperative client-side leases**, as in Doorman [9]. They remove the shared store from the hot
  path but trust clients to respect their grants. Here the lease holders are the limiter instances
  themselves, so the tokens are enforced and the bound holds by construction.
- **Starting chunks at the minimum** (8 tokens) and growing them. Only a dry lease doubles a chunk,
  and with early refills a steadily used lease is never dry, so a moderately busy key would stay near
  the minimum. Its checks would rarely wait, but one in eight would cost a Redis call. Starting at
  the cap and halving idle chunks keeps a key at 3 checks per second near 25 tokens: one Redis call
  per 25 checks.
