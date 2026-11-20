# Diagrams

Activity and sequence diagrams of the service, drawn with [Archify](https://github.com/tt-a1i/archify) from
the source at commit `4fc8dcd` (branch `feat/merchants`). Each box links to the files and lines it
describes. Open a page in a browser; nothing needs installing.

## Activity diagrams

| Diagram | What it shows |
| --- | --- |
| [Processing a check](activity-1-check.html) | a check through the service and the customer rule: key, plan, local gates, burst, quota, and Redis down |
| [The quota script decides](activity-2-quota-script.html) | `lua/quota.lua`: terms in force, period, retried request ids, the three overage policies, alerts |
| [A check served from a lease](activity-3-leased-check.html) | `LeasedCore`: a lease hit, a local denial, one shared refill, and the cases that take the exact path |
| [A Redis call when Redis fails](activity-4-redis-failure.html) | `ResilientBackend`: the breaker per slot group, deadlines, which calls are retried, the failure policy |

## Sequence diagrams

| Diagram | What it shows |
| --- | --- |
| [One check, end to end](sequence-1-check.html) | gateway, service, `CompositeCore`, `PlanStore` and Redis, call by call |
| [A merchant joins](sequence-2-merchant-joins.html) | the operator creates a merchant, every limiter loads its key, the merchant adds a plan and a customer, the first check |
| [A limit changed live](sequence-3-live-limit-change.html) | a customer out of quota is switched to postpaid; every instance drops its cached plan and denial at once |
| [A top-up, sent twice](sequence-4-topup-retry.html) | an idempotent top-up whose reply is lost; the retry gets the first answer and buys nothing more |
| [An alert reaches the merchant](sequence-5-alert-webhook.html) | `quota.lua` publishes, the notifier signs and delivers it, and retries a 503 |

## For the thesis

In a page, **Export** downloads the diagram as PNG or SVG (light, dark or both).

## Changing a diagram

Edit its source in `src/`, then render it again. Archify must run from Windows here, because WSL has
no browser for its checks:

```bash
"/mnt/c/Program Files/nodejs/node.exe" 'C:\Users\ADMIN\.agents\skills\archify\bin\archify.mjs' finalize \
  sequence 'docs\diagrams\src\sequence-1-check.json' 'docs\diagrams\sequence-1-check.html' \
  --repo-root 'D:\GitHub\distributed-rate-limiter' --quality showcase --out-dir '.archify\receipts'
```

The source references point at commit `4fc8dcd`. After the code changes, update `revision` and the line
numbers in the sources, or the render refuses them. The `*.delivery.json` files are Archify's
receipts; `archify check <page> --require-provenance` uses them to confirm a page is unchanged.
