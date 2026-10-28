# Control plane: management API and alert notifier

> Phase 2, P2 and P3 (docs/PRODUCT_PLAN.md) · Status: P2 done: `rlcontrol/api.py` (FastAPI),
> `rlcontrol/notifier.py`, the P2 gate end to end in `tests/test_e2e.py`. P3: billing (`rlcontrol/billing.py`)
> and the payment sandbox (`rlcontrol/payments.py`), and the capacity and check-rate settings, done;
> 28 tests (`run_tests.sh`)

The C++ limiter is the data plane: it answers checks. This folder is the control plane, which changes
what the limiter answers. The two meet only in Redis: the API writes plan hashes and announces each
change on `plan-changes`, exactly as `tools/rlctl` does, and every limiter applies it on the next call.

## Running it

```bash
control/setup.sh                      # control/.venv with the pinned packages (requirements.txt)
RL_ADMIN_TOKEN=<at least 16 chars> RL_REDIS=127.0.0.1:7001 \
  control/.venv/bin/uvicorn rlcontrol.api:app --app-dir control --port 8080     # OpenAPI page: /docs
RL_REDIS=127.0.0.1:7001 control/.venv/bin/python -m rlcontrol.notifier          # from control/
RL_TENANCY=merchants build/release/rate_limiter_server 0.0.0.0:50051 config/rules.yaml 127.0.0.1:7001
```

## The API

There are two callers, each with its own bearer credential.

**The operator** (`RL_ADMIN_TOKEN`):

| Method and path | What it does |
| --- | --- |
| `POST /v1/merchants` | creates a merchant; returns its check key (for its gateway, sent to the limiter as `x-api-key`) and its management key, each once |
| `POST`, `DELETE /v1/merchants/{id}/check-keys` | rotates or revokes the merchant's check keys; every limiter drops them at once |
| `PUT`, `GET`, `DELETE /v1/merchants/{id}/check-rate` | how many checks a second the platform takes from the merchant; beyond it every limiter answers `RATE_LIMITED` at once |
| `POST /v1/customers/reindex` | rebuilds every merchant's customer list from the plan hashes (customers made before the list, by `tools/rlctl` before it kept one, or across a crash) |
| `POST /v1/billing/close?month=YYYY-MM` | invoices an ended month and charges the customers with a card (below); `charge=false` only invoices; `merchant=` limits it |

**A merchant** (its management key):

| Method and path | What it does |
| --- | --- |
| `PUT`, `GET`, `DELETE /v1/plans/{name}` | its plan catalogue |
| `POST /v1/customers`, `GET /v1/customers/{id}` | a customer on a plan; its terms, boost, top-up and usage this period, alerts included |
| `GET /v1/customers?after=&limit=` | its customers in id order, a page at a time (up to 200), each as above |
| `GET /v1/usage` | this month's calls admitted and billed overage over all its customers, with Redis's clock: polled twice, the calls a second (refused calls are not counted) |
| `PUT /v1/customers/{id}/plan` | moves the customer to another plan (custom limits and boosts end; a paid top-up stays) |
| `PATCH /v1/customers/{id}/limits` | custom limits for one customer: quota, burst, period, policy, allowance, prices |
| `POST`, `DELETE /v1/customers/{id}/boost` | extra units for some minutes |
| `POST /v1/customers/{id}/topups` | prepaid units for the current period; an `Idempotency-Key` makes a retry buy nothing more |
| `POST /v1/customers/{id}/suspend`, `resume` | suspend without losing the numbers |
| `PUT`, `GET`, `DELETE /v1/capacity` | its backend's capacity in calls a second, from all its customers together, and the percentage `reserve` kept for plans with a fee |
| `PUT`, `GET`, `DELETE /v1/customers/{id}/payment-method` | the card its invoices are charged to, as the payment provider's token |
| `GET /v1/customers/{id}/invoice-preview` | this month's invoice so far |
| `GET /v1/invoices?month=YYYY-MM`, `/v1/invoices.csv?month=YYYY-MM` | its invoices of a month, as JSON or CSV |
| `PUT`, `GET`, `DELETE /v1/webhook` | where alerts go, and the secret they are signed with |

**The three answers at the quota.** A plan's `overage` decides:
- `hard`: blocks at the quota;
- `grace`: admits `allowance` more, free;
- `postpaid`: admits up to `allowance` more as billed overage. That allowance is the spending cap per
  quota period: a monthly plan's bill can never hold more overage than it, and a daily plan's no more
  than it times the days of the month.

Any customer can have its own policy and allowance (`PATCH .../limits`), a boost, or a top-up.

**Prices** are terms of a plan, like its quota, so a customer can have its own (all in cents):
`fee` a month, `overage_price` per 1,000 billed overage calls, `topup_price` per 1,000 top-up calls.

## The merchant portal

`/portal` (open it on the API's address) is a page for one merchant, signed in with its management key:
- **This month:** calls a second now, calls admitted, billed overage, customers (every 5 s).
- **Customers:** a page at a time, with each one's usage bar, billed overage and alerts; adding one.
- **One customer:** its terms, the invoice so far this month, and the actions: move to a plan, custom
  limits, a boost, a top-up (with a fresh idempotency key), suspend (after a confirmation) and resume.
- **Plans** (define, change, delete), **the capacity** and its reserve, **invoices** of a month with a
  CSV download, and **the webhook**.

It reads the API above and nothing else, so it can do exactly what the API allows. The page loads
nothing from other origins, under a strict Content-Security-Policy, and writes every value as text,
never as HTML. The key stays in the browser tab (`sessionStorage`) and is sent only to this server.
Refusals are not shown: the counters hold admitted calls (alerts report a customer's first refusal at
its quota).

`tests/eval/portal_ui.mjs` drives every action in headless Chrome (Node 22, Chrome on Windows by
default, `CHROME=<path>` otherwise) against a running API and a merchant seeded by
`tests/eval/portal_seed.py`; one line per check, exit 1 on a failure.

## Protecting the merchant's backend (P3)

`PUT /v1/capacity {"rate": 1000, "reserve": 20}` (an optional `burst`, default one second's worth):
- **The limit:** the limiter refuses a call beyond it with `RATE_LIMITED` and a retry time. It is taken
  after the customer's own burst and before its quota, so a refused call is never counted or billed.
  While Redis says the capacity is out, calls are refused from the lease before the burst check, so a
  flood over it costs Redis nothing.
- **The reserve:** 20% of it is only for plans with a fee, so a flood of Free calls cannot take it. A
  burst too small to leave a whole call outside the reserve is refused (422), with the least that works.
- **Where it is decided:** leased token buckets keyed by the merchant (`src/core/composite_core.cc`), so
  N limiters admit at most the capacity plus N x one lease chunk.

The operator's check rate (`/v1/merchants/{id}/check-rate`) works the same way but counts every check
of the merchant, before anything else, so that merchant's flood costs the others nothing. Both
settings reach every limiter at once (`!merchants` on `plan-changes`).

## Billing and payments (P3)

`rlcontrol/billing.py` writes one invoice per customer and calendar month:
- **What it bills:** the plan's fee, the month's billed overage at the plan's price, and each top-up
  bought that month at the price it was bought at. Each line is rounded half up to the cent, once.
- **Where the numbers come from:** the quota counter, never the usage events. `lua/quota.lua` keeps the
  month's totals in it atomically with every decision, and files a finished month that had billed
  overage until it is invoiced (kept 35 days past it).
- **Once:** an invoice's id names the customer and month, so closing a month again writes nothing
  twice; a customer with nothing to bill gets no invoice.
- **When:** after the month ends by Redis's clock, from cron or the operator's endpoint:
  `python -m rlcontrol.billing close [--month YYYY-MM]` (from `control/`; `export` and `reconcile` too).

**Payments.** The platform never holds a card number; one sent to the API is refused. The card goes
into the payment provider's form, which returns a token; the platform keeps only the provider's
customer id, and at close charges each open invoice, with the invoice as the idempotency key:
- **Paid:** the invoice is marked paid.
- **Declined:** the invoice is marked `payment_failed` and the customer suspended.
- **No card:** the invoice stays open, for the merchant to collect.

`rlcontrol/payments.py` has the provider: `SandboxProvider` stands in for a test mode (Stripe test
mode or a VNPay sandbox are still to settle) and takes Stripe's test tokens, `tok_visa` and
`tok_mastercard` paying, `tok_chargeDeclined` declined.

**Reconciliation.** `python -m rlcontrol.billing reconcile --month YYYY-MM <usage files>` compares each
customer's usage events (`RL_USAGE_FILE` of every limiter) with its counter and reports every
difference; it corrects none.

## Alerts

`lua/quota.lua` publishes one JSON message on `rl-alerts` the first time in a period that a customer:
- reaches 80% of its limit in force;
- reaches the limit;
- goes into billed overage;
- is denied.

The notifier posts it to the merchant's webhook with these headers:
- `X-RL-Event`;
- `X-RL-Delivery`, a stable id for dropping duplicates;
- `X-RL-Signature: t=<seconds>,v1=<HMAC-SHA256 of "<t>.<body>">` (check it with `webhooks.verify`).

Delivery is retried with backoff on errors, but not on a refusal (4xx). It is at most once while the
notifier is down, because Redis Pub/Sub keeps no messages. The same state is always readable from
`GET /v1/customers/{id}` (`usage.alerts`).

## Security

- **Keys:** only their SHA-256 hashes are stored (`{rl:merchants}:keys`, `{rl:merchants}:admin`).
- **Isolation:** the merchant comes from the credential, never from the URL, so no request can reach
  another merchant's customers.
- **Webhooks:** URLs that resolve to private, loopback, link-local or reserved addresses are refused,
  when registered and again before every delivery, and redirects are not followed. A delivery
  connects to the address it checked, never to a second lookup of the name, so DNS rebinding cannot
  turn it inward. This keeps a merchant from pointing the platform at its own internal network. `RL_ALLOW_PRIVATE_WEBHOOKS=1` lifts
  it for a demo on one machine.
- **Webhook secrets:** stored in Redis, since the notifier needs them to sign.

## Tests

`control/run_tests.sh`; each test skips when what it needs is not running.

| File | What it checks | Needs |
| --- | --- | --- |
| `test_periods.py` | period names and resets against `quota.lua` | the test Redis, 6380 |
| `test_api.py` | credentials, isolation, validation, plans, customers, custom limits, boosts, idempotent top-ups, suspension, webhook addresses, usage against the script, capacity and check rate | the dev cluster |
| `test_notifier.py` | delivery, signature, retries, refusals, private addresses | the dev cluster |
| `test_billing.py` | invoices from the counters, once; charges once, a decline suspends; no card numbers; this month's preview with the real script; CSV; reconciliation | the dev cluster |
| `test_e2e.py` | the P2 gate end to end (below); P3's capacity and reserve, and the check rate, through the real limiter | the dev cluster and `build/release/rate_limiter_server` |

The P2 gate (`test_e2e.py`), with the real limiter:
1. The three answers through gRPC.
2. Each changed live per customer, past the limiter's local denial.
3. The 11 alerts that follows, delivered and signed.
4. Another merchant's customer of the same id untouched.
5. A revoked key locked out.
