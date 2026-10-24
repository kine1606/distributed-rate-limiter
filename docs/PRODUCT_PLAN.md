# Phase 2: from a rate limiter to a rate-limiting product

> Plan written 2026-10-25 after the advisor's review. Four weeks, Oct 26 to Nov 22, plus two optional
> weeks, Nov 23 to Dec 6; delivery stays Dec 7 to 15 (ROADMAP Week 9). Decisions marked **[ask]** are
> to settle with the advisor before the week that needs them.

## 0. In one paragraph

The advisor's point: scaling to 1,000,000 checks per second is the easy part; the thesis must show what
the service is **for** and how it is **sold**. Today the project serves one API owner. Phase 2 turns it
into a product that many API owners (**merchants**) buy. Each merchant defines plans for its own users
with any numbers, changes a user's limit at any time, chooses what happens when a paying user hits the
limit in an emergency (block, a bounded grace, or pay later), is billed exactly for what was used, and
has its backend protected from overload. Most of the data plane already exists; the new work is the
merchant layer, a control plane, metering and billing, and capacity protection.

## 1. Where the project stands

**Is it one client today? In practice, yes.** The service already has many end users (tested with one
million) and many gateways and instances can call it, but they all belong to **one API owner**:

| Today | What it means |
| --- | --- |
| One rules file (`config/rules.yaml`) and one plan catalog (`tools/plans.sh`) | every customer chooses from the same plans |
| One key space (`{cust:<id>}:…`) | two API owners' user ids would collide |
| `Check(key, rule, cost, request_id)`, no credential | anyone who reaches the port can check or spend any customer's quota |
| `tools/rlctl create/upgrade/downgrade/suspend` | a customer can only move between fixed plans |
| Daily quotas, reset at 00:00 UTC | no monthly package |
| Overage policies hard, grace and cap per plan | the three emergency answers exist, but only as fixed plan settings |
| Usage events in a local file per instance | no invoice, no payment |

**What carries over unchanged:**
- **Correctness:** the Lua scripts (exact quotas, request ids, overage) and the leases with their N × C bound.
- **Speed:** the deny cache, and the plan cache with Pub/Sub invalidation (a change reaches every
  instance in about 22 ms).
- **Operations:** the failure policy per plan, metrics, the live view, and the whole evaluation tool
  chain.
- **A head start on custom limits:** each customer's plan hash already stores its own numbers (`daily`,
  `burst`, `refill`, `overage`, `extra`), so custom limits need an API, not a new storage model.

## 2. The product

**The roles.**

| Role | Example | Relationship |
| --- | --- | --- |
| Platform | us: the rate-limiting service | sells limiting, metering and protection to merchants |
| Merchant | person A, who runs a weather API | buys the service; defines plans; is billed by the platform |
| End customer | A's user `u42` on A's Pay plan | buys a plan from A; is limited and metered |
| Merchant backend | A's weather servers | protected from overload by the platform |

**What is sold, two different things.**
1. **Count limits (quotas):** how many calls each end customer may make per day or per month: Free 200
   a day, Pay 2,000. These are billed, so they must be exact.
2. **Protection:** how many calls per second reach the merchant's backend in total, whatever the
   customers' quotas. Quotas can be oversold (not every customer uses its quota at once), so the backend
   needs its own limit.

The current design already splits these two (an exact quota path and a leased rate path). Phase 2 adds
the merchant level on top.

**How a merchant connects.** A's gateway (or our gateway plugin, from `examples/gateway-python`) calls
`Check` with A's API key and the end customer's id, and gets allow or deny, the remaining quota and a
retry time. A manages plans and customers through a management API or a small web portal.

**How the platform earns.** For example: a monthly fee per merchant tier, plus a price per million
checks. The platform meters merchants the same way merchants meter their customers, and limits each
merchant's check rate so one merchant cannot slow the others. The prices are an illustration for the
thesis, not a business case.

**Positioning (to check before citing).** AWS API Gateway documents usage-plan quotas as best effort,
and Kong's Redis policy is exact only if it syncs every request (both already in the references). Usage-
billing products (for example Stripe metered billing, Lago, OpenMeter) bill usage after the fact but do
not limit in real time. The thesis's position: exact real-time quotas, metering that invoices exactly
what was admitted, and backend protection with a bounded error, in one service.

## 3. The emergency question: a paying customer hits 2,000 and needs more now

| Option | Customer experience | Merchant revenue | Risk | Already in the code |
| --- | --- | --- | --- | --- |
| 1. Block at the limit | outage at the worst moment | none beyond the plan | none | yes (`hard`) |
| 2. A bound, e.g. +5% free | survives small spikes, then blocked | none; gives 5% away | small, bounded | yes (`grace`, amount per plan) |
| 3. Allow, bill at the end of the period | no outage | overage revenue | unpaid bills; a runaway client spends without limit | half: `cap` counts billable overage exactly, up to `extra`; no invoice or payment |

**Recommendation:** make the policy a setting of each plan, overridable per customer, and support all
three, with option 3 made safe:
- **Free plans:** option 1.
- **Paid plans:** a small grace (option 2), then postpaid overage (option 3) up to a **spending cap**
  the merchant or customer sets, for example at most 2,000 extra calls or $20 a period. Beyond the cap,
  block. The cap is what makes "pay later" safe against a bug in the customer's client.
- **Alerts:** at 80% and 100% of the quota, when overage starts, and at the cap, as a webhook to the
  merchant.
- **Two self-service alternatives to postpaid:** a **prepaid top-up** (buy +1,000 now, applied at once),
  and a **temporary boost** (+N calls until a given time, granted by the merchant).

**The card.** "Bill to the bank card at the end of the month" must not mean storing card numbers.
Storing card numbers brings the service under PCI DSS. A payment provider stores the card; the platform
keeps only the provider's customer id and charges it when the invoice closes. In the thesis this is a
payment-provider sandbox, with test cards only **[ask: Stripe test mode, VNPay sandbox, or invoices
without payment]**. A failed payment suspends the customer (the `suspended` plan already exists).

Exactness carries over: overage is counted by the same atomic script as the quota, and a retried call
is not billed twice.

## 4. Protecting the merchant's backend

Quotas are counts per day or month; protection is calls per second. Example: A's backend handles 1,000
calls per second, but A has 10,000 customers with daily quotas. Their quotas never collide in a day, but
their bursts can.

- **A capacity limit per merchant:** one token bucket for all of A's traffic, served from leases. It is
  one hot key per merchant, which is exactly where leases are fastest (measured: 5.2 times strict on one
  hot key). It is checked after the customer's own limits, so it keeps the N × C bound.
- **Priority for paying customers:** the last part of the capacity bucket (a reserve, for example 20%) is
  only for paid plans. When A's backend is saturated, Free traffic is refused first, and paying customers
  keep at least the reserve.
- **Weights per endpoint:** an expensive endpoint costs more tokens (`cost` already exists in `Check`).
- **The platform protects itself too:** a check-rate limit per merchant, so one merchant's flood does not
  slow the others (the noisy-neighbour test, now between merchants).

## 5. Limits that change at any time

- **Plans per merchant:** each merchant has its own catalog in Redis, with any numbers, replacing the
  shared `tools/plans.sh`.
- **Overrides per customer:** any field of a customer's plan (quota, burst, policy, cap) can be set for
  that customer alone.
- **Boosts with an expiry:** +N calls until a time, for emergencies and promotions.
- **Effective at once:** a change publishes on `plan-changes`, as `rlctl` does today, and every instance
  applies it on the next call.
- **Periods:** the UTC day (today), and the calendar month for monthly packages **[ask: calendar month,
  or from the subscription date]**.

## 6. Architecture changes

```text
Merchant's gateway --Check(api key, customer, cost)--> Limiter instances (C++, data plane)
                                                         | customer limits -> merchant capacity
Merchant portal / API --REST--> Management service (control plane) --HSET + PUBLISH--> Redis Cluster
                                 Billing job: period close -> invoices -> payment sandbox
```

**Decisions, each with the alternative it rejects:**

| Decision | Choice | Rejected, and why |
| --- | --- | --- |
| Who the merchant is | taken from its API key (gRPC metadata; keys stored hashed) | a merchant field in the request: any caller could claim to be any merchant |
| Key space | `{m:<merchant>:c:<customer>}:…`, one hash tag per customer | one Redis database per merchant: Redis Cluster has only one database |
| Control plane | a separate, low-traffic service: Python and FastAPI, with OpenAPI docs | adding it to the C++ server: slower to build, and it would mix admin traffic into the hot path |
| Source of truth for billing | the quota counters, kept until the period is invoiced; Redis persistence (AOF) switched on | usage events: they are best effort today; counters are already exact and atomic |
| Postpaid overage | the existing `cap` policy, its `extra` becoming the spending cap | a new Lua path: same semantics, more code to prove |
| Capacity protection | a leased bucket per merchant, checked after the customer's limits | exact per-call checks: one hot key per merchant would serialise on one Redis master |

## 7. What changes in the thesis

**Title and claim.** For example, "A Multi-Tenant Rate Limiting and Usage Metering Service for API
Providers" **[ask]**. The claim adds: merchants are isolated; limits change at once; invoices equal
metered usage; the merchant's backend is protected, with priority for paying customers.

**New goals:**

| Goal | Statement | Test |
| --- | --- | --- |
| G10 | one merchant cannot read, change or slow another's limits | security and noisy-merchant tests |
| G11 | a limit change or boost applies on every instance within 1 s | change-latency test, as the plan-change test today |
| G12 | an invoice equals the metered usage exactly, retries included | a simulated month with retries and a killed master; any loss reported |
| G13 | postpaid overage never exceeds the spending cap | property tests with the reference model |
| G14 | the merchant's backend never receives more than its capacity plus N × C, and paid customers keep their reserve under a Free flood | capacity and priority test |

**Chapters:**
- **Ch1:** the business problem and the roles.
- **Ch3:** sections on tenancy, overage and billing, and capacity protection, each with decisions and
  rejected alternatives.
- **Ch4:** the control plane, billing and the portal.
- **Ch5:** the new tests.
- **Ch6:** results against G10 to G14.

All existing results stay valid.

## 8. Week by week

### Week P1 (Oct 26 to Nov 1): merchants and adjustable limits

- [ ] The business design, written as a Ch3 section, decisions and rejected alternatives first (4 h).
- [ ] Merchant identity: API keys in gRPC metadata, stored hashed, cached per instance; a wrong key is
      refused (6 h).
- [ ] Key space and catalogs per merchant; platform defaults replace `config/rules.yaml` for plans (6 h).
- [ ] Overrides per customer and boosts with an expiry, read by the scripts (6 h).
- [ ] Monthly periods in `quota.lua` beside the daily ones (5 h).
- [ ] Tests: Lua, unit, and isolation between two merchants (6 h).

Gate: two merchants with different catalogs on the same instances; a custom limit or a boost applies on
the next call on every instance; a key cannot reach another merchant's customers; all 240 existing
tests still pass.

### Week P2 (Nov 2 to 8): control plane and the emergency policies

- [ ] Management API (FastAPI, OpenAPI). Platform admin: merchants. Merchants: plans, customers,
      overrides, boosts, suspend, current usage. It writes Redis and publishes `plan-changes` (12 h).
- [ ] Policies per plan, overridable per customer: hard; grace; postpaid up to a spending cap; prepaid
      top-up (6 h).
- [ ] Merchant webhooks at 80% and 100%, at overage start and at the cap, sent from usage events, never
      on the request path (6 h).
- [ ] The property tests' reference model extended with overrides, boosts, monthly periods, postpaid
      and caps (6 h).

Gate: the advisor's three options shown end to end through the API, configurable per plan and per
customer, changed live; property tests equal the model.

### Week P3 (Nov 9 to 15): metering, billing, backend protection

- [ ] Durable metering: AOF on the cluster; counters kept until invoiced; reconciliation of counters
      against usage events (6 h).
- [ ] Billing job at period close: base fee plus overage times unit price, one invoice per customer and
      period, with an idempotent id; JSON and CSV export (10 h).
- [ ] Payment sandbox: provider customer and card token, charge at close, suspend on a failed payment.
      No card number stored (6 h; becomes invoices only if short of time).
- [ ] Merchant capacity bucket (leased), the paid-plan reserve, endpoint weights, and a check-rate limit
      per merchant (10 h).

Gate: one simulated month with retries and a killed master, where invoices equal the counters (any
difference reported, not hidden); the merchant capacity never exceeded beyond N × C; under a Free flood,
paying customers keep their reserve.

### Week P4 (Nov 16 to 22): portal, evaluation, thesis v2

- [ ] Merchant portal: one page on the management API. Plans, customers, adjust a limit or grant a boost,
      usage bars, invoice preview, and the live view filtered to one merchant (8 h).
- [ ] Evaluation (10 h):
  - isolation, and a noisy merchant;
  - billing exactness;
  - limit-change latency;
  - capacity and priority;
  - 10,000 merchants of 100 customers each: throughput and Redis memory;
  - the cost of tenancy against today's numbers.
- [ ] Thesis v2: Ch1, Ch3, Ch4, Ch5 and Ch6 updated; G10 to G14 in the goal tables; rebuilt with
      `docs/thesis/build.sh` (12 h).
- [ ] Demo v2: merchant A defines Free and Pay; a Pay customer in an emergency goes over, into postpaid,
      and the invoice preview shows it; a Free flood is shed while paying customers are unaffected (3 h).

Gate: every new claim has an evidence file in `docs/thesis/evidence/`; thesis v2 assembled.

### Optional Week P5 (Nov 23 to 29): if the business layer runs long

- [ ] Billing periods from the subscription date, and proration when a customer upgrades mid-period.
- [ ] Payment webhooks and retries; a Vietnamese provider's sandbox if the faculty prefers one.
- [ ] Hardening: key rotation, a limit on failed authentication, an audit log of every limit change.
- [ ] The AWS run, if the budget is agreed.

### Optional Week P6 (Nov 30 to Dec 6): buffer

- [ ] Fixes found by the evaluation; slides and defense sheet v2; a mock defense with the new questions.

Then Dec 7 to 15: the advisor's review and delivery, as planned (ROADMAP Week 9).

**Cut first, if late:**
1. Payment sandbox: invoices only.
2. The portal: the API with its OpenAPI page only.
3. Subscription-date periods: calendar month only.
4. Webhooks: a usage endpoint the merchant polls instead.

**Never cut:** isolation (G10), billing exactness (G12), the spending cap (G13).

## 9. Risks

- **Scope.** Billing and payments can absorb any amount of time. The thesis claims exact metering and
  invoices, not a payment business, so payment stays a sandbox.
- **Billing under failover.** Redis replicates asynchronously, so a killed master can lose recent
  increments. The plan measures and reports the loss; it does not hide it.
- **Security.** Credentials, isolation between merchants, and no card data become part of the claim.
  Each gets a test.
- **Speed.** Authentication and the merchant capacity check add work to every call. Measured against
  today's numbers in P4; the merchant bucket is leased, so it should cost about one lease hit.

## 10. To settle with the advisor

1. The default for paying customers in an emergency: postpaid up to a cap (recommended), or a grace.
2. Payment: Stripe test mode, VNPay sandbox, or invoices without payment.
3. Monthly periods: the calendar month, or from the subscription date.
4. Is a web portal needed for the defense, or is the API with its OpenAPI page enough?
5. The new title.
