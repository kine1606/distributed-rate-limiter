# Chapter 3. Design

> Started Week 2 (2026-10-18) with the product decisions (3.1.3); the lease protocol and its bound
> (3.5) added in Week 3. The rest of the chapter (3.1 to 3.8 in `OUTLINE.md`) is written in Week 5
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
