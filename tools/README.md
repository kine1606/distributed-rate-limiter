# Admin tools

> Week 2 (seed script), Week 4 (rlctl, usage report) · Flow steps B1, B5, B6, B9 · Status: `seed_plans.sh` done (plan catalogue at its top; 20,010 plans in 0.3 s); `rlctl` and usage report not started

## What this module does

These tools play the account service from Part B of the flow, which is out of scope as a real
product. They write plans into Redis and announce changes, so you can demonstrate sign-up, upgrade,
downgrade and suspension without building a billing system. Shell or Python is fine here; they are
not on the hot path.

## Files to create

| File | Purpose | Week |
| --- | --- | --- |
| `seed_plans.sh` | Create many customers on Free, Pay and Enterprise for tests and benchmarks | 2 |
| `rlctl` (Python or bash) | `create`, `upgrade`, `downgrade`, `suspend`, `show` for one customer | 4 |
| `usage_report.py` | Sum usage-event files per customer across instances | 4 |
| `plots/*.py` | Turn `results/` files into thesis figures | 7, 8 |

## Skeleton

```bash
tools/seed_plans.sh 10000      # {cust:1..10000} free, {cust:pay:1..10000} pay, {cust:ent:1..10} enterprise
REDIS_PORT=7002 tools/seed_plans.sh 1000000   # any cluster node works as the entry point
```

```text
rlctl create   cust:42 --plan free
rlctl upgrade  cust:42 --plan pay        # HSET plan hash, then PUBLISH plan-changes cust:42
rlctl downgrade cust:42 --plan free      # same; "end of cycle" is simulated by running it later
rlctl suspend  cust:42
rlctl show     cust:42                   # plan hash + today's quota hash
```

## Plan presets

| Plan | daily | burst | refill | overage | extra | failure |
| --- | --- | --- | --- | --- | --- | --- |
| free | 200 | 2 | 2 | hard | 0 | closed |
| pay | 2000 | 20 | 20 | grace | 200 | open |
| pay_cap | 2000 | 20 | 20 | cap | 1000 | open |
| enterprise | 1000000 | 2000 | 2000 | cap | 100000 | open |

The enterprise preset exists for the hot-key test: one customer heavy enough to keep every
limiter instance busy, which is where the lease accuracy bound matters.

## Hints

- **`redis-cli --pipe` does not follow cluster redirects.** `seed_plans.sh` sends the whole command
  stream to every master instead: each master writes the keys of its own slots and answers `MOVED`
  for the rest, which `--pipe` counts as errors. With 3 masters that is 3 times the commands, still
  far faster than `redis-cli -c`, which reconnects on every redirect (about 3,000 keys/s measured).
  The script then reads back one key of each customer group to decide success.
- **Publish after writing.** `rlctl upgrade` must `HSET` first and `PUBLISH` second. Reversed, an
  instance could reload the old plan before the new one is written.
- **Keep plots reproducible.** Each plot script reads raw files from `results/` and writes a PNG or
  PDF. Never edit a figure by hand; regenerate it.

## Done when

- [x] Week 2: one command seeds the test customers.
- [ ] Week 4: `rlctl upgrade` applies on all instances within one second (with the plan cache).
- [ ] Week 4: `usage_report.py` totals used by the accuracy test.
