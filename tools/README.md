# Admin tools

> Week 2 (seed script), Week 4 (rlctl, usage report) · Flow steps B1, B5, B6, B9 · Status: not started

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
# seed_plans.sh  REDIS=127.0.0.1:7001  COUNT=1000000
# HINT: use redis-cli -c (cluster mode) and pipe commands for speed:
#   for i in $(seq 1 $COUNT); do
#     echo "HSET {cust:$i}:plan plan free daily 200 burst 2 refill 2 overage hard extra 0 failure closed"
#   done | redis-cli -c -h ... -p ... --pipe        # HINT: --pipe does not follow MOVED; see below
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

- **`redis-cli --pipe` does not follow cluster redirects.** For a million keys either write to each
  master the keys of its slots (compute the slot of `{cust:N}`), or use a small Python script with
  `redis-py`'s `RedisCluster` and pipelines, which handles routing for you. The Python route is
  simpler and fast enough.
- **Publish after writing.** `rlctl upgrade` must `HSET` first and `PUBLISH` second. Reversed, an
  instance could reload the old plan before the new one is written.
- **Keep plots reproducible.** Each plot script reads raw files from `results/` and writes a PNG or
  PDF. Never edit a figure by hand; regenerate it.

## Done when

- [ ] Week 2: one command seeds the test customers.
- [ ] Week 4: `rlctl upgrade` applies on all instances within one second (with the plan cache).
- [ ] Week 4: `usage_report.py` totals used by the accuracy test.
