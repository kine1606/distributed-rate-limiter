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

Done 2026-10-25 on branch `feat/merchants`:

- [x] The business design, written as a Ch3 section (`docs/thesis/ch3_design.md` 3.9), with decisions and
      rejected alternatives first.
- [x] Merchant identity: API keys in the gRPC metadata `x-api-key`, stored hashed (SHA-256) in
      `{rl:merchants}:keys`, loaded at start and on `!merchants`. Resolved with no lock, by a per-thread
      cache dropped on every change; a wrong or missing key gets `UNAUTHENTICATED`. Switched on with
      `RL_TENANCY=merchants` (`src/server/merchants.h`).
- [x] Key space and catalogues per merchant: the service names merchant `acme`'s customer `cust:42`
      `acme|cust:42`, so every key, plan, lease and denial is the merchant's own. A merchant's plans live
      in `{rl:m:<merchant>}:catalog`; the platform's plans (`tools/plans.sh`) remain the defaults.
      `config/rules.yaml` keeps the rules, which are the same for every merchant.
- [x] Custom limits per customer (`rlctl set`) and boosts with an expiry (`rlctl boost`), read by the
      script, which decides by the Redis clock. Applying a plan replaces the hash atomically.
- [x] Monthly periods in `quota.lua` (`period month`, UTC calendar month), beside the daily ones.
      Monthly and boosted plans take the exact path.
- [x] Tests, 13 new (253 in all):
  - Lua: a monthly quota, and a boost;
  - unit: parsing; the key index (hashing, resolving, revoking, a failed reload, concurrency); the
    service refusing unknown keys and prefixing single and batch checks;
  - integration: the end-to-end gRPC path with a key loaded from Redis; two merchants' catalogues,
    a boost and a custom limit on two instances.

Gate status: met.
- *Two merchants, own catalogues, same instances:* daily quotas 2 and 3, the same customer id counted
  apart.
- *A boost and a custom limit:* each reached both instances within one second, past a denial both held
  locally.
- *Refusal:* a call without a merchant's key is refused.
- *The real server (`RL_TENANCY=merchants`):* a key added and then revoked with `rlctl` worked, then was
  refused at once.

### Week P2 (Nov 2 to 8): control plane and the emergency policies

Done 2026-10-25 on branch `feat/merchants`:

- [x] Management API (`control/rlcontrol/api.py`, FastAPI, OpenAPI page at `/docs`).
  - *The operator:* merchants, and rotating or revoking their keys.
  - *A merchant:* plans, customers, custom limits, boosts, top-ups, suspension, usage, its webhook.
  - *How:* every write is one atomic script on the customer's hash, then a `plan-changes` message.
  - *Credentials:* a separate management key per merchant, hashed; the merchant always comes from the
    key, never from the URL.
- [x] Policies per plan, overridable per customer: hard; grace; postpaid up to a spending cap (`cap`,
      named `postpaid` in the API); and prepaid top-ups.
  - *Top-ups:* bought for the current period only, kept across plan changes, and idempotent with an
    `Idempotency-Key`.
  - *For billing:* each purchase is recorded in `{rl:m:<merchant>}:purchases`.
- [x] Alerts:
  - *What:* `lua/quota.lua` publishes on `rl-alerts` at 80%, at the limit, at the first overage and
    at the first denial, each once a period, and never for a replay.
  - *Delivery:* `control/rlcontrol/notifier.py` sends them to the merchant's webhook: signed
    (HMAC-SHA256 with a timestamp), retried with backoff, not retried on a 4xx.
  - *Safety:* private and internal addresses are refused, at registration and before each delivery.
  - *The cost:* at most once while the notifier is down; the same state stays readable through the API.
- [x] The property tests' model extended with monthly periods, boosts, top-ups and alerts:
  - *Coverage:* 2,000 seeds per configuration, 571,131 checks each, 1,138 month rollovers.
  - *Proof they can fail:* three planted bugs are caught. Writing them exposed a gap: the histories
    never changed a boost alone, so new operations now extend or end a boost, or add a top-up, alone.

Gate status: met (`control/tests/test_e2e.py`, with the real limiter):
- *The three answers through gRPC:* hard 10 then blocked; grace 10 + 1 free; postpaid 10 + 3 billed,
  then blocked.
- *Changed live per customer through the API,* each past the limiter's local denial: hard switched to
  postpaid, a boost, a top-up.
- *All 11 alerts* delivered and signed.
- *Isolation:* another merchant's customer of the same id untouched; a revoked key locked out.
- *Property tests* equal the model.

Tests: 4 new in C++ (257 pass in release, ASan and TSan; the property tests extended) and 18 in
`control/run_tests.sh`.

### Week P3 (Nov 9 to 15): metering, billing, backend protection

Done 2026-10-25 on branch `feat/merchants` (not committed yet):

- [x] Durable metering: AOF on the cluster; counters kept until invoiced; reconciliation of counters
      against usage events (6 h).
  - *AOF:* `appendfsync everysec` in `docker/docker-compose.yml` and the fault cluster; the dev
    cluster, which benchmarks flush, stays without.
  - *Counters:* `lua/quota.lua` keeps the calendar month's totals in each quota counter, atomically
    with the decision: units within the plan and billed overage, whatever the quota period.
    - A month with billed overage is filed as `closed:m<month>` when the next begins.
    - Its counter is kept 35 days past the month, until invoiced. A counter with nothing to bill
      still expires after its period, so a flood of unknown ids costs no more memory than before.
  - *Reconciliation:* `python -m rlcontrol.billing reconcile` compares usage events with the counters
    and reports every difference.
  - *Tests:* 5 new script tests against Redis. The property tests' model now states the billing totals:
    2,000 seeds per configuration, 2,661 customer months with billed overage, 585 filed by a new month.
    Five planted bugs are caught (two in the script, three in its transcription).
- [x] Billing job at period close: base fee plus overage times unit price, one invoice per customer and
      period, with an idempotent id; JSON and CSV export (10 h).
  - *Billing period:* the calendar month, for daily quotas too.
  - *Prices:* terms of the plan, in cents, overridable per customer: `fee`, `overage_price` and
    `topup_price` per 1,000 calls. A top-up is billed at the price recorded when it was bought.
  - *Running it:* `control/rlcontrol/billing.py`, from cron or `POST /v1/billing/close`.
  - *Output:* JSON and CSV. A cell that could run as a spreadsheet formula is quoted.
- [x] Payment sandbox: provider customer and card token, charge at close, suspend on a failed payment.
      No card number stored (6 h; becomes invoices only if short of time).
  - *Provider:* `control/rlcontrol/payments.py`, a sandbox that takes Stripe's test tokens. Stripe test
    mode or VNPay can replace it once settled with the advisor.
  - *Card numbers:* refused by the API. The platform keeps only the provider's customer id.
  - *Charging:* at close, idempotent by invoice. A decline suspends the customer; a customer without a
    card keeps an open invoice for the merchant to collect.
  - *Tests:* 7 new (`control/tests/test_billing.py`); five planted bugs are caught.
- [x] Merchant capacity bucket (leased), the paid-plan reserve, endpoint weights, and a check-rate limit
      per merchant (10 h).
  - *Settings:* the merchant sets its capacity (`PUT /v1/capacity`, rate, burst and the reserve
    percentage); the operator sets each merchant's check rate. Every limiter reloads both at once on
    `!merchants`.
  - *Where it is decided:* `CompositeCore`, from leased token buckets keyed by the merchant.
    - The check rate is taken first, for any rule. A lease hit is one CAS, with no allocation.
    - The capacity is taken after the customer's burst and before its quota, so a refused call is
      never counted or billed.
    - A plan with a fee may take the reserve; a Free plan never may.
  - *Endpoint weights:* the check's `cost`, which the gateway sets per endpoint; capacity is taken by
    cost. No new code.
  - *Tests:*
    - 8 engine tests, 3 for merchant loading and 1 for plans.
    - A property harness: 2 to 4 instances, a Free flood at 3 times the capacity, and paying customers
      asking for 70% of the reserve.
    - An end-to-end test through the real limiter.
    - Five planted bugs are caught: a Free plan taking the reserve, a refusal keeping the burst token,
      a check rate that never refuses, capacity switched off, and the reserve lost in the split.
    - One planted lease bug (a refill past one chunk) is caught by the existing leased-quota harness,
      not this one: its bound sums both buckets' lease slack.

Gate: one simulated month with retries and a killed master, where invoices equal the counters (any
difference reported, not hidden); the merchant capacity never exceeded beyond N × C; under a Free flood,
paying customers keep their reserve.

Gate status: met (`docs/thesis/evidence/p3_gate_2026-10-25.md`).
- *Billing:* `tests/eval/billing_gate.py`, 1,500 calls a second for 20 s over two limiters, with gateway
  retries by request id; one run with no kill and four with a master killed.
  - *Invoices:* equal the counters for every customer in every run, and the counters equal what the
    gateway was told. No increment was lost to a failover in these runs; the asynchronous gap remains
    possible and would show as a difference.
  - *Usage events:* 3 to 17 overage calls short in three of the kill runs, explained call by call: a
    script that answered after its deadline was logged as unavailable, and its retry was replayed.
    This is why billing reads the counters.
  - *The month:* it cannot end during a test, so the invoice is this month's preview, from the same
    function that closes a month.
- *Capacity:* 2,000 seeds, 2 to 4 instances, Free calls at 3 times the capacity. No window exceeded
  its bound (the fullest reached 91%), no Free call reached the reserve, and paying customers kept
  all 303,203 calls they asked for. Repeated through the real limiter in `control/tests/test_e2e.py`.

Tests: 277 in C++, all passing in release, ASan and TSan (20 new: 5 script, 8 engine, 5 merchant, 1
plan, 1 property); 30 in `control/run_tests.sh` (12 new).

A review of P1 to P3 then fixed:
- *Merchant tables leaked (P1, worse with P3's settings):* every reload kept its table until exit,
  about 3 MB per change at 10,000 merchants. Merchants are now made once per set of limits, and a
  replaced table is freed when no lookup reads it.
- *Reload order (P1):* an older reload finishing after a newer one could bring back a revoked key; the
  one started last now wins.
- *DNS rebinding (P2):* a delivery resolved the webhook's name again after the check; it now connects
  to the address checked.
- *Allocation (P3):* a merchant over its check rate cost an allocation per refused check; the lease
  now refuses it alone.
- *`tools/rlctl` (P1, P3):* it refused the price fields, and its merchants were missing from the list
  billing reads.
- *Wording (P2):* the postpaid allowance is a cap per quota period, so a daily plan's month holds up
  to the cap times its days. Prices are those in force at close (a `shortcut:` until proration, P5).
- *A test depending on timing (P3):* the end-to-end capacity bounds now follow the measured time.
Three fixes have a test shown to fail without them: merchants made once per set of limits, the
reload order, and DNS rebinding. Freeing a replaced table rests on `std::shared_ptr`, not a test. The
`rlctl` fix was checked by hand on the dev cluster. The allocation fix has no test of its own: the
behaviour it keeps is tested, the absence of an allocation is not.

A Phase 1 audit (`ponytail-audit`, Weeks 1 to 8 as they stand now) then fixed:
- *Shutdown (P1 in `main.cc`):* Redis was stopped while the plan-change listener still ran, and a
  `!merchants` message in that window reloaded merchant keys through the freed client. The listener
  now stops first.
- *Plain-text API keys (P1):* the server now takes TLS from `RL_TLS_CERT` and `RL_TLS_KEY`, and warns at
  start when tenancy runs without it. Tested with the real server (`control/tests/test_tls.py`), shown
  to fail when the server ignores the certificate.
- *Two stale comments:* `std::function` callbacks stay, as the Week 7 profile decided; `AllowAllCore`
  is the benchmark baseline, not a stand-in.

A security pass over Weeks 1 to 4 then fixed:
- *The example gateway trusted a client header (`X-User-Id`) as the user:* any client could escape its
  limit with a new id each time, or spend another user's quota. It now limits the user of an API key
  it knows (unknown key: 401) and a request without a key by its address
  (`control/tests/test_gateway.py`, shown to fail on the old code).
- *Docker published Redis, which has no password, on every interface:* the dev and fault clusters and
  the three test limiters now listen on `127.0.0.1` only (checked from Windows; WSL still reaches them).
- *The rule name had no length limit:* now 128 bytes, like the key and the request id.
- *The gateway sent internal gRPC errors (with addresses) to clients:* they now go to its log.

Known and left as documented: a limiter without tenancy trusts whoever reaches its port, by design
beside its gateway; and an over-burst flood costs Redis one script per call (the noisy-neighbour
test, Chapter 5), whose local fix was left out at the feature freeze.

The same pass over Weeks 5 to 8 then fixed:
- *Bug planting erased uncommitted work:* `tests/eval/planted_bugs.py` restored each file with
  `git checkout`, dropping every uncommitted change in it (the P3 engine changes, on this run). It now
  writes back the file as it was; a run on the uncommitted tree left it byte for byte unchanged.
- *A planted bug had stopped running:* one mutation's text drifted from the code on Oct 24, so the
  script skipped it in silence; the thesis's ten, measured on Oct 23, were right. The text is fixed,
  all ten are caught again, and the script now fails when one is not planted or not caught.
- *Two benchmarks took a Redis port from the environment and flushed it:* `bench/redis_functions.sh`
  and `bench/ceiling_compare.sh` now refuse any Redis that saves its data (the system Redis on 6379
  does; the throwaway instances run with `save ""`). The others find their masters through `cluster
  nodes`, which a plain Redis refuses.
- *Downloads were not checked:* `scripts/setup.sh` checks ghz and grpcurl against their projects'
  SHA-256 and redis-plus-plus against its commit; `control/setup.sh` gets a pinned pip checked against
  its SHA-256 instead of the latest `get-pip.py`; `control/requirements.txt` pins every package pip
  installs, dependencies too.
Checked and fine: the metrics endpoint and the live view listen on loopback, the live page writes
fetched text only as text, and `dashboard/data.js` is JSON-encoded.

### Week P4 (Nov 16 to 22): portal, evaluation, thesis v2

- [x] Merchant portal: one page on the management API. Plans, customers, adjust a limit or grant a boost,
      usage bars, invoice preview, and the live view filtered to one merchant (8 h).
  - *Done (2026-10-27 and 28):*
    - A customer list per merchant, kept by the API and `tools/rlctl`, rebuilt by
      `POST /v1/customers/reindex`, and read a page at a time by `GET /v1/customers`.
    - `GET /v1/usage`: the merchant's calls admitted this month, summed from the quota counters, so
      every limiter's calls are in it; polled twice it gives calls a second (the live view).
    - The page at `/portal`, served by the API under a strict CSP: the month's numbers, the customer
      table, one customer's terms and invoice so far with every action (plan, custom limits, boost,
      top-up, suspend after a confirmation, resume), plans, the capacity, invoices with a CSV download,
      the webhook. `tests/eval/portal_ui.mjs` drives all of it in headless Chrome: 22 of 22 checks.
  - *Not done:* a live count of refusals, which needs a per-merchant count in the limiter (the counters
    hold admitted calls); `/v1/usage` reads every customer's counter, marked as a shortcut.
- [x] Evaluation (10 h), `docs/thesis/evidence/p4_merchants_2026-10-28.md`, Chapter 5.12:
  - isolation: 9 of 9 checks; a planted shared namespace fails 4;
  - a noisy merchant: floods of unknown keys, or over the check rate, capacity or quota, cost Redis at
    most 1,440 calls a second; a burst flood still costs one script a call unless the check rate is set;
  - billing exactness: the P3 gate rerun, 3 runs (2 with a master killed), no customer with a difference;
  - limit-change latency: in force on both of two limiters within 6.6 ms of the API's answer;
  - capacity and priority: at most 1,003 a second against 1,000; paying customers keep the reserve;
  - 10,000 merchants of 100 customers: 193 bytes of Redis a customer, 25,749 calls a second, a new
    merchant's key working in 37 ms;
  - the cost of tenancy: within the noise (+2.1% and −3.9% medians, alternating runs).
  - *Found and fixed:* a flood over a merchant's capacity cost Redis two calls per refused call and
    slowed other merchants; it is now refused from the lease before the burst script (42,000 Redis calls
    a second down to 12,400, before and after in alternating runs). Also fixed: `billing_gate.py` waited
    on the seed node when that was the master it killed.
- [x] Thesis v2: Ch1 (roles, the merchant problem, G10 to G14, scope), Ch3 (3.9.8 the portal), Ch4 (4.7
      merchants and the control plane), Ch5 (5.12), Ch6 (G10 to G14, limitations, future work), both
      abstracts; rebuilt with `docs/thesis/build.sh` (12 h).
- [x] Demo v2: merchant A defines Free and Pay; a Pay customer in an emergency goes over, into postpaid,
      and the invoice preview shows it; a Free flood is shed while paying customers are unaffected (3 h).
      `scripts/demo_v2.py` (`KEEP=1` leaves the portal running).

- [x] Reviews of the P4 code (2026-10-28): ponytail-review, security-review, simplify and code-review.
  - *Fixed:*
    - a flood over a merchant's capacity cost Redis two calls per refused call (the noisy-merchant test);
    - a wholly reserved capacity still ran the burst script for every Free call;
    - a capacity whose burst leaves no whole call outside its reserve made the limiter refuse every Free
      call; the API now refuses it (422) with the least burst that works;
    - in the portal: paging errors were swallowed, the CSV download skipped the sign-out on a rejected
      key and revoked its link at once, and Suspend and Delete acted on one click;
    - `demo_v2.py` left its servers running on SIGTERM;
    - the portal check's seeder lived in `/tmp`, now `tests/eval/portal_seed.py`;
    - duplicated code in the lease denial, the load generator's key reading and the evaluation's seeding.
  - *Kept, with a `shortcut:` comment:*
    - a call the quota refuses keeps its capacity token, about one per customer, instance and period;
    - merchant rules are never freed, so ~10^6 capacity changes by one merchant would fill the lease
      table;
    - `/v1/usage` reads every customer's counter.
  - *Kept, judged not worth the change:*
    - check-rate refusals are not usage events (they are counted in the service's metrics);
    - a check naming an unknown rule spends a check-rate token before it is refused;
    - a check-rate lease miss probes the lease table twice;
    - the capacity step allocates closures, as the customer rule's other steps already do; the lease hot
      path of leased rules is unchanged.
  - *Security review:* nothing found in the P4 changes.

Gate: every new claim has an evidence file in `docs/thesis/evidence/`; thesis v2 assembled.
*Gate met (2026-10-28):* the new claims of Chapter 5.12 are in `p4_merchants_2026-10-28.md` (and the P3
gate's file); `thesis.docx` and `thesis.pdf` rebuilt (85 pages). 279 C++ tests pass in release, ASan and
TSan, the 39 of the control plane pass, and the property tests pass at 2,000 seeds. Not committed yet.

### Optional Week P5 (Nov 23 to 29): if the business layer runs long

- [x] Billing periods from the subscription date, and proration when a customer upgrades mid-period.
  - *Done (2026-10-29), as an option, so either answer to section 10's question 3 is a setting:*
    `PUT /v1/billing {"periods": "subscription"}` starts the months of customers created from then on on
    their own day (plan field `anchor`, 1 to 28; `quota.lua` ARGV 10; the property tests' transcription
    and model, which found that a billing month must never move back). A fee changed mid-month is
    prorated by time, from changes recorded by the API and `tools/rlctl`.
  - *Tests:* 2 C++ (parse, the script on a real Redis), property tests at 2,000 seeds with 87,620 checks
    on anchored plans and 547 months ended on an anchor day, 4 Python and the script check extended; 5
    planted bugs caught.
- [x] Payment webhooks and retries; a Vietnamese provider's sandbox if the faculty prefers one.
  - *Done (2026-10-29):* a declined charge is retried after 1, 3 and 7 days and suspends only after the
    last; a pending charge is settled by the provider's signed webhook (`POST /v1/payments/webhook`,
    HMAC-SHA256 with a timestamp, events handled once). 2 tests, 3 planted bugs caught.
  - *Not done:* a VNPay sandbox, which needs a merchant account and the faculty's preference. VNPay pays
    on a redirect page, not by charging a saved customer at month close, so it would add a pay-by-link
    flow beside the sandbox rather than replace it.
- [x] Hardening: key rotation, a limit on failed authentication, an audit log of every limit change.
  - *Done (2026-10-29):* management keys rotated by the merchant; check keys revoked one at a time by id;
    20 failed authentications a minute per client address, then 429; every successful change through
    the API or `tools/rlctl` in the merchant's audit stream, with secrets masked (`control/README.md`).
    4 API tests, each shown to fail with its bug planted.
  - *Not done:* a failed-key limit on the limiter's gRPC side (a wrong key costs a hash and no Redis
    call, measured in P4); an operator's view of a merchant's log (it reads the stream).
- [ ] The AWS run, if the budget is agreed. *Not done: it needs an AWS account and an agreed budget.*

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
