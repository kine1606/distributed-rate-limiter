# Evidence: property-based tests against a reference model (Week 6, 2026-10-23)

The Week 6 item: "random sequences of checks, retries, upgrades, downgrades and day rollovers, run
through `CompositeCore` on the fake backend and through a trivially correct in-memory model; every
answer must match; seeded so a failure replays" (`docs/THESIS_PLAN.md`).

## What runs

`tests/property_test.cc`, part of the unit tests, so it runs in release, ASan and TSan.

- **The system under test** is the real engine: `CompositeCore` over `StrictCore`, or over
  `LeasedCore` as `main.cc` builds it, with the real `PlanStore` and `DenyCache`.
- **The fake Redis** executes the rules of the three scripts the customer path uses (`token_bucket.lua`,
  `quota.lua`, `return_lease.lua`). It works in milli-tokens, whole milliseconds and UTC days on a fake
  clock, keeps request-id answers for five minutes, and expires idle buckets the way `RedisPPBackend`
  sets their lifetime. The scripts' own arithmetic is tested against a real Redis by
  `lua_scripts_test.cc` (33 tests). This test is about everything the engine composes on top of them.
- **The reference model** states the product rules of Chapter 3, 3.4 as one sequential function per
  check, in the order the chapter gives them:
  1. a suspended account is denied;
  2. a customer this instance saw run out of quota is denied until the reset, under the same quota terms;
  3. the burst bucket;
  4. the daily quota of the UTC day under the plan's policy, with request ids;
  5. a quota denial gives the burst token back.

  During an outage, the plan last read applies (with the cache) or Free does (without it), and the
  plan's failure policy answers. The model shares no code with the engine.
- **A history** has 200 to 600 random steps over three customers:
  - checks of cost 1 to 3, sometimes up to 12, 70% of them with a request id;
  - retries of earlier ids, most soon after the original, some much later, a tenth with a different cost;
  - plan changes: random plans, suspension, a deleted hash, or a hash with some fields missing;
  - Redis outages, short and rare;
  - clock jumps of milliseconds to minutes, including exactly at a stored answer's five minutes and
    within ±1.5 s of the UTC reset;
  - rollovers into the next UTC day.

  After every check, the engine's answer and every usage event must equal the model's, field by
  field: allowed, reason, remaining, retry time, limit, reset time, replayed.

## Properties

1. **Exact plans equal the model** in four configurations:

   | | strict engine | leased engine |
   | --- | --- | --- |
   | plan cache (5 s) and plan-change messages, as `PlanListener` delivers them | ✓ | ✓ |
   | no plan cache and no messages, so a plan change reaches the deny cache only through its stamp of the quota terms | ✓ | ✓ |

2. **Leased quota keeps its bounds** (3.4.5). Plans of 100,000 to 250,000 a day are served from
   leases on 1 to 3 instances sharing the fake Redis, under one of the three policies. A lease admits
   differently from the exact path, so the property is about bounds rather than equality:
   - each day Redis grants no more than the plan (plus grace; overage at most the cap);
   - over the whole history, the instances admitted exactly what Redis granted, minus at most N × C
     still held in leases;
   - no day admits more than its own grants plus N × C carried over from the day before.

## Result

20,000 seeds per configuration (`RL_PROPERTY_SEEDS=20000`, 70 s on one core):

| Property | Histories | Checks | Answers covered |
| --- | --- | --- | --- |
| Exact plans equal the model, each of 4 configurations | 20,000 | 5,645,991 | `OK` 1,608,156, `RATE_LIMITED` 2,034,892, `QUOTA_EXHAUSTED` 1,024,556, `OVERAGE` 133,772, `SUSPENDED` 522,603, `UNAVAILABLE` 213,715, `UNVERIFIED` 108,297; 243,793 replayed answers; 797,990 plan changes, 144,966 outages, 34,425 day rollovers |
| Leased quota bounds | 20,000 | about 108 million | 80,490 days, 32,701 of them at the plan's limit; 9.09 billion units admitted; 1,969,215 granted but still in leases at the end |

The numbers are those of the configuration with the plan cache. Without it the outage answers
differ, as they should: an unknown plan is Free and fails closed.

Every history of every configuration matched. The default run in the test suite is 300 seeds per
configuration, about 1.1 s in release.

## Does the test find bugs? Ten planted in the engine

A test that has never failed proves little, so ten bugs were planted in the engine's source, one at a
time. Each was built, run through 300 seeds per configuration, and reverted from git:

| Bug planted | Caught |
| --- | --- |
| a burst token not given back when the quota denies | yes |
| a replayed denial cached in the deny cache | yes |
| the deny cache skipped (burst checked first) | yes |
| a denial surviving a change of the plan's quota terms | yes (by the configurations without messages) |
| a denial lasting one second past the reset | yes, after the generator was given steps that land within ±1.5 s of the reset; the first version jumped hours past midnight and missed it |
| an invalidated plan left in the per-thread plan copies | yes (configurations with the plan cache) |
| no stale plan during an outage | yes (configurations with the plan cache) |
| an outage on the burst check failing closed for every plan | yes |
| a quota lease chunk allowed to grow to twice the cap | yes: a day admitted 100,236 against 100,000 granted |
| a lease refill adding a chunk instead of topping up | yes: 391,699 admitted against 392,031 granted, more than N × C held |

Script: `tests/eval/planted_bugs.py`. Each bug is a single line changed in the engine.

## Limits

- The fake Redis is a transcription of the scripts, so a bug shared by a script and its
  transcription would go unseen here. The scripts are tested against a real Redis separately.
- One process, one thread. Concurrency is covered by the TSan runs of the other tests, the seeded
  lease harness and the 4-instance evaluation tests.
- The plan cache's staleness after a missed message is deliberate (up to the five-second TTL), so
  every configuration either delivers every message or uses no cache.
