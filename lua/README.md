# Lua scripts

> Weeks 1, 4, 5 · Flow steps A10, B5, B6, B7 · Status: `token_bucket.lua`, `fixed_window.lua`, `sliding_window.lua` and `quota.lua` done and tested

## What this module does

Each script is one atomic decision inside Redis. Redis runs a script start to finish without
running any other command in between, so two limiter instances can never both take the last token.
That atomicity is the whole reason for using Lua instead of `GET` then `SET` from C++.

## Inventory

| Script | Purpose | Week | Status |
| --- | --- | --- | --- |
| `token_bucket.lua` | Burst limit; also serves lease refills through `requested` | 1 | done |
| `quota.lua` | Daily quota per UTC day, stores `used` | 1 | done |
| `quota.lua` (extended) | Overage policies `hard`, `grace`, `overage_cap` | 4 | to do |
| `dedupe` logic | Request ID: count a retried request once | 4 | to do |
| `return_lease.lua` | Give unused leased tokens back on shutdown | 5 | to do |
| `fixed_window.lua` | Counter per epoch-aligned window; cheapest, up to 2x limit across an edge | 2 | done |
| `sliding_window.lua` | Sliding window *counter* (current + weighted previous window), O(1) | 2 | done |

## Rules every script follows

These rules come from the project spec and Redis Cluster. Breaking one of them either breaks
Cluster routing or makes results wrong under load.

- **Only touch keys passed in `KEYS`.** Redis Cluster routes a script by its keys. A key built
  inside the script may live on another node, and the script then fails with `CROSSSLOT` or
  silently writes to the wrong place.
- **All keys of one call share one hash tag,** for example `{cust:42}:quota` and
  `{cust:42}:req:abc`. The part in braces decides the slot, so they land on the same node.
- **Time comes from `redis.call('TIME')`.** All instances then agree on "now", and day rollover
  happens once. Redis 7 replicates script effects, so `TIME` before a write is allowed.
- **O(1) only.** No loops over data, no `KEYS *`, no `SCAN`. A slow script blocks the whole Redis
  node for everyone.
- **Integers for state.** Tokens are stored as milli-tokens so repeated refills never drift.
- **Every write refreshes a TTL** (`PEXPIRE`), so idle customers cost no memory forever.

## Contracts (already implemented)

```text
token_bucket.lua
  KEYS[1] = {cust:42}:burst:tb          (src/redis/keys.h builds every key)
  ARGV    = capacity, refill_per_sec, requested, ttl_ms
  returns   { allowed 0|1, remaining, retry_after_ms }

fixed_window.lua / sliding_window.lua
  KEYS[1] = {user:42}:<rule>:fw   /   {user:42}:<rule>:sw
  ARGV    = limit, window_ms, requested
  returns   { allowed 0|1, remaining, retry_after_ms }   (TTL computed inside from the window)

quota.lua
  KEYS[1] = {cust:42}:daily_quota:q
  ARGV    = daily_limit, cost
  returns   { allowed 0|1, used, remaining, reset_after_ms }
```

## Week 4: overage policies

Extend `quota.lua` instead of writing a second script, so one call still makes one decision.

```text
quota.lua (v2)
  KEYS[1] = {cust:42}:daily_quota:q
  ARGV    = daily_limit, cost, policy ("hard" | "grace" | "cap"), extra
            -- extra = grace amount (e.g. 10% of limit) or overage cap
  state     HASH { day, used, overage_used }
  returns   { allowed, used, remaining, reset_after_ms, overage 0|1 }
```

Build it in this order:

1. Keep the current path untouched when `used + cost <= limit`. Run the Week 1 tests first; they
   must still pass, which proves you did not break the normal case.
2. When the limit is reached, branch on `policy`. `hard` denies as today. `grace` allows while
   `used + cost <= limit + extra`. `cap` allows while `overage_used + cost <= extra` and increments
   `overage_used`, because overage is billed separately from the plan.
3. Reset `overage_used` together with `used` when the day changes.
4. Return `overage = 1` for every request admitted beyond the limit, so the usage event and the
   response can mark it.
5. Add tests: request 2,001 on three customers gives deny, allow, and allow with `overage = 1`;
   the cap is respected exactly; rollover resets both counters.

Hint: pass the policy and numbers in from the plan instead of reading the plan hash inside the
script. The script stays a pure function of its arguments and is easier to test, and an upgrade
still applies on the next request because the caller passes the new values.

## Week 4: request ID

A gateway that times out and retries must not be charged twice.

```text
inside quota.lua (v2), before counting:
  KEYS[2] = {cust:42}:req:<request_id>      -- same hash tag as KEYS[1]
  if request_id given:
      if EXISTS KEYS[2]: return the stored result (do not count again)
      after deciding:   SET KEYS[2] <encoded result> PX 300000
```

Hint: store the result, not just "seen". A retry must get the same answer as the original,
including `allowed = 0` if it was denied. Only do this for quota checks (strict path). Burst checks
from the lease are not deduplicated, and you should say so in the design document.

## Week 5: return_lease.lua

On graceful shutdown each instance gives back tokens it leased but never used, so a restart does
not burn part of a customer's burst.

```text
return_lease.lua
  KEYS[1] = {cust:42}:burst:tb          (src/redis/keys.h builds every key)
  ARGV    = capacity, tokens_to_return
  effect    tokens = min(capacity * 1000, tokens + returned * 1000)
```

Hint: this is best effort. If Redis is down at shutdown, the tokens are lost, which only makes the
limiter slightly stricter, never looser.

## How to test a script quickly by hand

```bash
redis-cli SCRIPT LOAD "$(cat lua/quota.lua)"          # prints the SHA
redis-cli EVALSHA <sha> 1 '{c1}:quota' 3 1            # limit 3, cost 1
redis-cli HGETALL '{c1}:quota'
```

## Done when

- [x] Week 1: `tests/lua_scripts_test.cc` passes against real Redis.
- [ ] Week 4: overage and request ID tests pass; the old tests still pass.
- [ ] Week 5: `return_lease.lua` tested (tokens never exceed capacity).
- [ ] Every script has a header comment explaining each step (spec requirement).
