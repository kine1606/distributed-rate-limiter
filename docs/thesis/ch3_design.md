# Chapter 3. Design

> Started Week 2 (2026-10-18) with the product decisions (3.1.3). The rest of the chapter (3.1 to
> 3.8 in `OUTLINE.md`) is written in Week 5 from `docs/DESIGN.md`. Section numbers may shift then.

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
