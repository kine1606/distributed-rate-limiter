# Control plane: management API and alert notifier

> Phase 2, P2 (docs/PRODUCT_PLAN.md) · Status: done: `rlcontrol/api.py` (FastAPI), `rlcontrol/notifier.py`,
> 18 tests (`run_tests.sh`), the P2 gate end to end in `tests/test_e2e.py`

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

**A merchant** (its management key):

| Method and path | What it does |
| --- | --- |
| `PUT`, `GET`, `DELETE /v1/plans/{name}` | its plan catalogue |
| `POST /v1/customers`, `GET /v1/customers/{id}` | a customer on a plan; its terms, boost, top-up and usage this period, alerts included |
| `PUT /v1/customers/{id}/plan` | moves the customer to another plan (custom limits and boosts end; a paid top-up stays) |
| `PATCH /v1/customers/{id}/limits` | custom limits for one customer: quota, burst, period, policy, allowance |
| `POST`, `DELETE /v1/customers/{id}/boost` | extra units for some minutes |
| `POST /v1/customers/{id}/topups` | prepaid units for the current period; an `Idempotency-Key` makes a retry buy nothing more |
| `POST /v1/customers/{id}/suspend`, `resume` | suspend without losing the numbers |
| `PUT`, `GET`, `DELETE /v1/webhook` | where alerts go, and the secret they are signed with |

**The three answers at the quota.** A plan's `overage` decides:
- `hard`: blocks at the quota;
- `grace`: admits `allowance` more, free;
- `postpaid`: admits up to `allowance` more as billed overage. That allowance is the spending cap: no
  bill can grow past it.

Any customer can have its own policy and allowance (`PATCH .../limits`), a boost, or a top-up.

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
  when registered and again before every delivery, and redirects are not followed. This keeps a
  merchant from pointing the platform at its own internal network. `RL_ALLOW_PRIVATE_WEBHOOKS=1` lifts
  it for a demo on one machine.
- **Webhook secrets:** stored in Redis, since the notifier needs them to sign.

## Tests

`control/run_tests.sh`; each test skips when what it needs is not running.

| File | What it checks | Needs |
| --- | --- | --- |
| `test_periods.py` | period names and resets against `quota.lua` | the test Redis, 6380 |
| `test_api.py` | credentials, isolation, validation, plans, customers, custom limits, boosts, idempotent top-ups, suspension, webhook addresses, usage against the script | the dev cluster |
| `test_notifier.py` | delivery, signature, retries, refusals, private addresses | the dev cluster |
| `test_e2e.py` | the P2 gate end to end (below) | the dev cluster and `build/release/rate_limiter_server` |

The P2 gate (`test_e2e.py`), with the real limiter:
1. The three answers through gRPC.
2. Each changed live per customer, past the limiter's local denial.
3. The 11 alerts that follows, delivered and signed.
4. Another merchant's customer of the same id untouched.
5. A revoked key locked out.
