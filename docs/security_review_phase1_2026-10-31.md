# Security review of Phase 1 (2026-10-31)

A review of the limiter itself, done while you were out. Scope:
- the gRPC service and its request validation;
- how Redis keys are built;
- the Lua scripts;
- the server's defaults and its connections to Redis;
- the metrics endpoint and the usage file;
- the Docker image and compose file;
- the admin tool;
- the example gateway.

It follows the two earlier passes over Weeks 1 to 8, recorded in `docs/PRODUCT_PLAN.md` under "security passes". Every finding here is new.

## Findings and fixes

| # | Severity | Finding | Fixed in |
| --- | --- | --- | --- |
| F1 | Medium | A customer key containing `{` or `}` breaks how Redis Cluster places one customer's keys together; a paid plan could then be used without being counted | `src/server/convert.h`, `tools/rlctl`, `proto/rate_limiter.proto` |
| F2 | Medium | The limiter listened on every network interface by default (`0.0.0.0:50051`), although in single-tenant mode it authenticates no caller | `src/server/main.cc` |
| F3 | Medium | Nothing could give Redis a password or an ACL user, so Redis could only be protected by network isolation | `src/redis/redis_auth.h` (new), `redispp_backend.cc`, `plan_listener.cc`, `control/rlcontrol/store.py` |
| F4 | Low | The Docker image ran the limiter as root | `docker/Dockerfile` |
| F5 | Low | The usage-events file was created readable by every user on the machine | `src/usage/usage_log.cc` |
| F6 | Low | A metrics client that never read its answer could stall the metrics thread | `src/metrics/exposer.cc` |
| F7 | Low | The example gateway listened on every interface by default, with public demo API keys | `examples/gateway-python/gateway.py` |

### F1: a key could split one customer's data across Redis masters

**The problem.** Every Redis key is built as `{<customer>}:<rule>:...`. Redis Cluster decides which master holds a key from the text between the first `{` and the next `}`. Normally that is the whole customer id, so all of one customer's keys land on the same master, and the quota script can update several of them at once.

A customer id starting with `}` leaves that text empty: `{}x}:...`. Redis then hashes the whole key instead, so one customer's quota key and its request-id key land on different masters. The script then fails with `CROSSSLOT`, which the limiter reads as "Redis is unavailable". For a plan whose failure policy is "open" (paid plans), every such call is then admitted without being counted.

**Proven on the dev cluster.** The quota script with the keys of a customer named `}x`:

```
redis-cli -c -p 7001 EVAL "$(cat lua/quota.lua)" 2 '{}x}:daily_quota:q' '{}x}:daily_quota:req:r1' 100 1 hard 0 day
CROSSSLOT Keys in request don't hash to the same slot
```

The same call for a customer named `x` answers `1`.

**The fix.**
- The service now refuses any key containing `{` or `}` with `INVALID_ARGUMENT`.
- `tools/rlctl` refuses such customer ids too.
- The proto file states the rule.
- The management API's customer pattern already excluded braces.

**Test:** `Convert.AKeyCannotCarryAHashTag`.

### F2: no authentication, yet listening on every interface

**The problem.** Without `RL_TENANCY=merchants`, the limiter does not authenticate callers at all. Anyone who reached its port could check any customer's key, and so use up that customer's quota, or probe plans. Its default address was nonetheless `0.0.0.0:50051`.

**The fix.**
- The default is now `127.0.0.1:50051`.
- Any address other than loopback logs a warning at start, unless tenancy is on: "listening on … with no caller authentication: whoever reaches it can check, and so use up, any customer's quota".
- Docker's image still passes `0.0.0.0:50051` explicitly, and compose already publishes the ports on 127.0.0.1 only.

**Verified:**
- `0.0.0.0:50072` logs the warning.
- The default listens on `127.0.0.1:50051`, without a warning.

### F3: the limiter could not authenticate to Redis

**The problem.** Neither the Redis client pool nor the plan listener's connection could send credentials, so a production Redis could not require `AUTH` or ACL users. Its only protection was being unreachable from outside.

**The fix.**
- `RL_REDIS_USER` and `RL_REDIS_PASSWORD` now apply to every connection the limiter makes. They are read in one header, `src/redis/redis_auth.h`, which both connection points use.
- The Python control plane (API, notifier, billing job) now connects through one helper, `store.connect()`, which reads the same variables.
- `redis-cli` and `tools/rlctl` use Redis's own `REDISCLI_AUTH` variable.

**Verified** against a Redis 7.4 that requires a password, run in Docker:

| Limiter started with | Answers | Admitted without being counted | `NOAUTH` errors in its log |
| --- | --- | --- | --- |
| no password | 7,079 | 7,079 (the rule fails open) | 5 |
| the password | 6,400 | 0 | 0 |

### F4 to F7

- **F4:** the image now creates a `limiter` user (uid 10001) and runs as it. The limiter needs no privilege: its port is above 1024 and it writes no files by default. Not tested by building the image.
- **F5:** the usage file is opened with mode `0600` (owner only) when it is created. Verified: the new file is `600`.
- **F6:** the metrics socket has a send timeout, alongside its existing receive timeout.
- **F7:** the gateway example now defaults to `127.0.0.1:8080`. `scripts/demo.sh` already passed that address.

## Checked and found fine

- **Request limits:** key, rule and request id at most 128 bytes, batches at most 1,000 checks. An unknown rule is refused. Cost is a uint32, and both the scripts (Lua numbers) and the leases (int64) handle its full range.
- **Metrics labels:** only rules in the config get a label set, so made-up rule names cannot create new time series.
- **The metrics endpoint:** it listens on loopback by default, has a receive timeout, and serves one fixed path.
- **Lua scripts:** they touch only the keys they are given, never use `KEYS`, `SCAN` or `FLUSH`, and read time only from Redis.
- **Elsewhere:**
  - the limiter's ports in compose are published on 127.0.0.1 only;
  - downloads in `scripts/setup.sh` are checked against their SHA-256;
  - benchmarks refuse a Redis that saves its data;
  - the live demo view listens on loopback.

## Recommended, not done

- **Caller authentication without tenancy.** Use `RL_TENANCY=merchants` (API keys) or a network policy. Client certificates (mutual TLS) would be the stronger option for one API owner.
- **TLS between the limiter and Redis.** redis-plus-plus supports it, but it is not wired in.
- **Redis passwords in the deployment compose.** Its Redis has none, and is reachable only on the compose network. It needs `--requirepass` before being exposed anywhere else.
- **gRPC is Ubuntu's 1.51 package.** Check that Ubuntu's security updates include the fix for HTTP/2 "rapid reset" (CVE-2023-44487). `RL_MAX_STREAMS` (100 concurrent calls per connection) already limits the damage on our side.
- **Python dependencies.** They are pinned, but no vulnerability audit (`pip-audit`) has been run on them.

## Tests

- **New test:** `Convert.AKeyCannotCarryAHashTag`. It fails with the brace check removed (planted, then
  restored).
- **C++:** 282 of 282 pass in release, AddressSanitizer and ThreadSanitizer.
- **Control plane:** 48 of 48 pass.
- **Hand checks:** F2, F3 and F5 were verified by hand, as described above. F4 was not: the image was not
  built.
- **Uncommitted on purpose:** `CLAUDE.md` gained the new default address, but stays uncommitted with your
  earlier edits there.

## For this afternoon

**State:** everything above is committed locally and **not pushed**. The laptop was shut down when the review was finished.

Next, in order:
1. **Look at this file and push.** Say "push", or run `git push origin feat/merchants`.
2. **Slides v2.** The 15-slide deck predates merchants. It needs the Phase 2 story, the G10 to G14 results, and P5. Claude can build it as a slide deck that downloads as `.pptx`.
3. **The AWS run,** when your account is set up (`aws/README.md`). It is about 2 USD and the one measurement the thesis still lacks.
4. **The advisor's five decisions** (`docs/PRODUCT_PLAN.md`, section 10).
5. **Your parts of the front matter:** student ID, the advisor's name, and the acknowledgments.

Docker Desktop was started for this review. It starts again with Windows, or by hand.
