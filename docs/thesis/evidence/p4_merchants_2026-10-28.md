# Evidence: merchants evaluated on the real limiter (P4, 2026-10-28)

The P4 evaluation of `docs/PRODUCT_PLAN.md`: isolation and a noisy merchant (G10), the latency of a limit
change (G11), billing exactness (G12), the spending cap (G13), capacity and priority (G14), 10,000
merchants, and the cost of tenancy. Also the merchant portal's end-to-end check and demo v2.

Code: the working tree on top of commit `bd24b60` (branch `feat/merchants`), not committed when
measured. Laptop as in Table 5.1: load generator, limiters and Redis share 12 hardware threads. Limiters
are pinned to CPUs 0-5 and load generators to 6-11 (`taskset`); Redis runs in Docker, unpinned.

Scripts:
- `tests/eval/tenancy_eval.py` (run with `control/.venv/bin/python`): sections `isolation`, `noisy`,
  `latency`, `capacity`, `scale`, `cost`. Raw output: `results/2026-10-28_tenancy/<section>.json`;
  the before-and-after runs of the capacity change are `ab_noisy_*` and `ab_cost_*` there.
- `tests/eval/billing_gate.py`: raw output `results/2026-10-28_billing_gate/` (`run_*.log` per run).
- Every limiter runs `RL_TENANCY=merchants`, engine `leased`, `config/rules.yaml`, on the dev cluster
  (3 masters, no persistence), which the scripts FLUSHALL before every run.

## 1. Isolation (G10)

Two merchants, each with a customer `cust-1` on a plan of 5 calls a day, through one limiter and the
management API. All 9 checks pass:

| Check | Result |
| --- | --- |
| A's customer: 5 within its quota of 5, then refused | pass |
| B's customer of the same name is untouched | pass |
| each merchant's API sees only its own customer's usage (5 and 1) | pass |
| A's management key cannot see B's customer (404) | pass |
| A's management key cannot change B's customer (404) | pass |
| a key naming B's customer (`shop-b|cust-1`) under A's API key does not reach B's customer | pass |
| an unknown API key is refused (`UNAUTHENTICATED`) | pass |
| A's check key does not manage A (401) | pass |
| a revoked check key is refused at once | pass |

**The checks can fail:** with a planted bug that names every merchant's customer in one namespace
(`TenantKey` writing `shared|` instead of the merchant), 4 of the 9 fail (the first, second, third and
sixth). The 5 that pass are decided by the API or the key index, which the bug does not touch; the API's
own isolation tests are in `control/tests/test_api.py`.

## 2. A noisy merchant (G10)

As evaluation test 11 (`noisy_neighbour_2026-10-23.md`), with merchants: another merchant's 1,000 Pay
customers at 5,000 checks a second (open loop, 10 s after 1 s of warm-up), while the noisy merchant sends
20,000 a second to one customer from 1 s before. Redis calls: `EVALSHA`, `EVAL` and `HGETALL` a second over
all three masters, during the other merchant's run (its own checks cost 2 each: about 10,000).

| Noisy merchant | Others' p50 ms | Others' p99 ms | Others' p999 ms | Redis calls/s | Noisy merchant's answers |
| --- | --- | --- | --- | --- | --- |
| none | 1.12 (1.12 to 1.17) | 4.66 (2.05 to 10.17) | 10.17 | 10,135 | — |
| unknown API key | 2.14 (1.88 to 2.66) | 14.39 (3.92 to 34.22) | 37.32 | 10,115 | 260,000 `UNAUTHENTICATED` |
| over the operator's check rate (100/s) | 1.65 (1.65 to 1.88) | 3.76 (3.30 to 8.93) | 5.79 | 10,281 | 1,398 admitted, 258,602 refused |
| over its own capacity (100/s) | 1.65 (1.65 to 1.88) | 3.44 (3.30 to 23.17) | 5.55 | 11,575 | 1,395 admitted, 258,605 refused |
| over its customer's daily quota | 1.72 (1.58 to 1.72) | 6.32 (3.16 to 11.09) | 11.09 | 10,136 | 260,000 `QUOTA_EXHAUSTED` |
| over its customer's burst (20/s), within quota | 2.23 (2.14 to 3.16) | 16.38 (13.19 to 92.68) | 28.77 | 30,144 | 240,081 refused, 19,642 `UNAVAILABLE` (run 1) |

Medians of 3 runs, range in brackets. The other merchant's calls were all answered and all allowed
(50,000 of 50,000 in every run), but in one over-burst run 75 of them were admitted unverified: Redis was
slow enough that their plan's failure policy (open) answered.
- **Free for Redis:** an unknown key, a flood over the check rate, over the capacity, or over the quota.
  The rise in the others' latency there is the laptop's shared CPU (the limiter answers 20,000 more calls a
  second).
- **Not free:** a flood over one customer's burst within its quota. Each call runs the burst script, as in
  test 11 (3.6.1). The operator's check rate closes it: set, it holds Redis at the baseline (row 3).

### The capacity change, before and after

The first measurement (same script, the build before the change) found the capacity row like the burst
row: each call over the capacity ran the customer's burst script and returned its token, two Redis calls,
and the other merchant's calls slowed, some answered by their failure policy (277, 3 and 63 admitted
unverified, and 3 `UNAVAILABLE`, in three runs; `noisy_build_before_capacity_change.json`). The change:
while Redis says the capacity is out, a call is refused from the lease before the burst script
(`LeasedCore::DeniedLocal`, `CompositeCore::CapacityOut`; test
`MerchantLimitsTest.OnTheLeasedEngineAFloodOverCapacityCostsRedisNothing`, which fails with the early
refusal planted out: 6 burst scripts instead of 1). Alternating runs of the two builds:

| Build | Redis calls/s (3 runs) | Others' p50 ms | Others' p99 ms | Others admitted unverified |
| --- | --- | --- | --- | --- |
| before | 41,655, 42,539, 42,342 | 5.09, 5.09, 4.47 | 44.38, 42.50, 44.38 | 0, 0, 0 |
| after | 12,489, 12,721, 12,072 | 2.44, 2.54, 2.05 | 12.63, 27.55, 14.39 | 0, 1, 0 |

Its cost on every check of a merchant with a capacity (one more lease lookup), as the `tenancy+limits`
variant of section 7 in alternating runs: before 32,567 checks/s median (25,803 to 36,628), after 30,141
(28,408 to 32,870). The ranges overlap, and the laptop's noise is ±10%: no difference is shown either way.
The capacity property test (2,000 seeds) gives the same totals as before the change, since the early
refusal denies only what the lease would deny.

## 3. Latency of a change (G11)

Two limiters; a merchant's customers `c-big` (1,000,000 a day: a leased quota) and `c-small` (3 a day, past
it: the deny cache holds it); 10,000 checks a second of background load on the first limiter. Each change
is made through the management API (in process, on the dev cluster); then both limiters are checked in turn
until each answers with the change. 30 trials per change.

| Change | API call, p50 (ms) | In force on both limiters after the API answered: p50 / p99 / max (ms) | API call and propagation, max (ms) |
| --- | --- | --- | --- |
| suspend | 8.0 | 2.19 / 2.86 / 2.86 | 12.8 |
| resume | 7.5 | 3.16 / 4.02 / 4.02 | 13.3 |
| quota down (leased quota) | 7.8 | 3.74 / 5.82 / 5.82 | 15.9 |
| quota up | 7.8 | 3.21 / 4.92 / 4.92 | 13.1 |
| boost on (customer denied) | 7.8 | 4.09 / 6.57 / 6.57 | 14.9 |
| boost off | 7.6 | 4.02 / 5.52 / 5.52 | 14.8 |
| capacity on (1 a second) | 6.8 | 1.20 / 5.77 / 5.77 | 14.6 |
| capacity off | 5.9 | 2.24 / 4.93 / 4.93 | 11.1 |

No limiter answered under the old terms after the API answered, except for the capacity: 1 or 2 answers in
3 of 30 trials (on) and 7 of 30 (off). A capacity reaches the limiters by a reload of the merchant table
(three hash reads), not by a cache drop, and the first answer under a new bucket of 1 may rightly admit.
(With one limiter and the build before the capacity change, the first batch: max 63.7 ms.)

## 4. Billing exactness (G12)

`tests/eval/billing_gate.py` rerun on today's code, as in `p3_gate_2026-10-25.md`: one merchant, 40
postpaid customers, 1,500 calls a second for 20 s over two limiters on the fault cluster (3 masters with
replicas, AOF every second), gateway retries with the same request id, and a master killed 8 s in.

| Run | Master killed | Calls | Told: in plan / overage | No decision after 6 tries | Counters: in plan / overage | Invoice overage, total | Events: in plan / overage | Customers where the counter differs | where the invoice differs |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| no kill | none | 29,997 | 6,000 / 23,997 | 0 | 6,000 / 23,997 | 23,997, $772.03 | 6,000 / 23,997 | 0 | 0 |
| kill 1 | 7106 | 29,999 | 6,000 / 23,184 | 815 | 6,000 / 23,184 | 23,184, $771.60 | 6,000 / 23,184 | 0 | 0 |
| kill 2 | 7105 | 29,996 | 6,000 / 23,744 | 252 | 6,000 / 23,744 | 23,744, $771.90 | 6,000 / 23,744 | 0 | 0 |

The kill runs retried 1,531 and 1,242 calls and answered 10,495 and 6,053 `UNAVAILABLE` on the way; a
call with no decision after 6 attempts was not counted, not billed and not recorded. A fix to the script
came first: it waited for the cluster by asking the seed node, which in one run was the master killed.

## 5. The spending cap (G13)

The property tests at 2,000 seeds per configuration (`RL_PROPERTY_SEEDS=2000`, every `Property.*` test),
on today's code: all 5 pass.
- *Exact plans against the model,* in four configurations (strict and leased engine, with and without the
  plan cache): 571,482 checks each, every answer and usage event equal to the model's. Each has 10,031
  answers of billed overage (`cap` plans, the API's `postpaid`), with 2,661 customer months holding
  billed overage, 585 of them filed when a new month began.
- *Leased quota bounds:* 8,146 days, 3,260 at the plan's limit; in none did Redis grant overage beyond
  the cap.
- **They catch a cap violation:** planted in the script's transcription, letting billed overage go one
  call past the cap (`c.over + cost - in_plan <= extra + 1`), all four model tests fail at seed 2 (a call
  answered `OVERAGE` that the model refuses). The script itself is tested against a real Redis
  (`tests/lua_scripts_test.cc`).

## 6. Capacity and priority (G14)

One merchant with a capacity of 1,000 calls a second, burst 1,000, 30% reserved for plans with a fee
(so 700 a second for every plan, 300 only for paid ones). 100 Free customers flood at 5,000 a second; 10
paying customers ask for 100, 300 or 600 a second; open loop, 10 s measured after 2 s (the buckets start
full). Medians of 3 runs, admitted calls a second:

| Load | Free admitted | Paid admitted / asked | Total |
| --- | --- | --- | --- |
| 1 limiter, paid 100/s | 686.6 | 100.0 / 100 | 786.6 |
| 1 limiter, paid 300/s | 656.4 | 300.0 / 300 | 956.4 |
| 1 limiter, paid 600/s | 624.7 | 378.5 / 600 | 1,003.2 |
| 2 limiters, paid 300/s | 654.9 | 300.0 / 300 | 954.9 |

- **The capacity holds:** at most 1,003.2 a second over 10 s, within the capacity plus what leases hold.
  The property test checks every window from 1 ms to 4 s against the bound (`property_tests`, section 5).
- **Free never takes the reserve:** at most 687 a second of its 700.
- **Paying customers keep the reserve, not more:** asking for 300 or less, they lost nothing; asking for
  600, they got the 300 of the reserve and about 78 more from the shared part, where their calls compete
  with Free calls. The reserve is a guarantee of that much, not a priority over Free in the shared part.

## 7. Many merchants, and the cost of tenancy

**10,000 merchants of 100 customers each** (20 Pay, 80 Free; 1,000,000 plan hashes, each merchant's key,
catalogue and customer index), written as the API writes them:
- *Seeding:* 7.1 s with `redis-cli --pipe`. Redis: 9.2 MB before, 202.6 MB after: **193 bytes a customer**,
  the merchant's share included.
- *A limiter:* started and loaded the 10,000 keys in 0.10 s (first check accepted); 118 MiB resident after
  the load below.
- *Load:* closed loop 12 channels × 16, a random merchant's key and customer per call (1,000,000
  customers, uniform), 12 s after 3 s: **25,749 calls/s** median (23,367 to 25,977), p50 7.19 ms, p99
  14.39 ms; no error. Nearly every call is a customer not seen within the plan cache's 5 s, so it pays a
  plan read besides its burst and quota scripts. After the load: 298.1 MB, 1,385,463 keys.
- *A new merchant:* its key worked **36.7 ms** (median of 10; 31.1 to 38.8) after `!merchants` was
  published, with 10,000 others loaded.

**The cost of tenancy:** as `bench/ab_compare.sh`'s customer cell (the customer rule over Zipf 10,000 Pay
customers, closed loop 12 × 16), in alternating runs of one build: without tenancy; with it (one merchant,
an API key on every call, the customer named `m1|cust:pay:N`); and with a capacity and a check rate too,
set too high to refuse (so every check also takes both merchant buckets). 5 runs each:

| Variant | Checks/s, median (range) | vs without | p50 ms | p99 ms |
| --- | --- | --- | --- | --- |
| without tenancy | 36,009 (31,631 to 38,240) | — | 5.09 | 13.78 |
| tenancy | 36,760 (25,243 to 37,483) | +2.1% | 5.09 | 11.59 |
| tenancy, capacity and check rate | 34,614 (30,529 to 37,383) | −3.9% | 5.09 | 14.39 |

About 52% of the calls are admitted in every variant (the Zipf head is over its burst of 20 a second). The
differences are within the laptop's noise of ±10%, and runs 4 and 5 were slower for every variant: tenancy
costs less than this setup can measure.

## 8. The merchant portal and demo v2

- `tests/eval/portal_ui.mjs` (headless Chrome over the DevTools protocol) against the API and a seeded
  merchant with 30 customers: 22 of 22 checks pass. It signs in, opens a customer, grants a boost, buys a
  top-up (on the invoice preview), sets custom limits, is refused an empty change, suspends after a
  confirmation and resumes, defines a plan, adds a customer, sets the capacity, is refused a private
  webhook address, reads an empty month of invoices and downloads its CSV, with no script error and no
  sideways scroll at phone width.
- `scripts/demo_v2.py`: a Pay customer 30 calls past its quota of 100 got 100 `OK` and 30 `OVERAGE`, and
  its invoice preview showed the $19.00 fee plus 30 billed calls ($0.02); with a capacity of 100 a second
  (30% reserved), a Free flood at 800 a second was held to 78 a second (70 plus the bucket's first full
  second) while a paying customer at 10 a second kept 80 of 80.

## What this does not show

- **One laptop:** throughput is a lower bound, and the latency of a change includes no network between
  the API, Redis and limiters on separate machines.
- **Two limiters** for the latency of a change and the capacity; the bound for N instances is the
  property test's.
- **The noisy merchant's burst flood still costs Redis** unless the operator sets its check rate.
