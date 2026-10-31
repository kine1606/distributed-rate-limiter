# Mock defense: how to run it (P6)

A rehearsal of the real defense, with someone playing the committee: a friend, the advisor, or Claude
in a chat. The answers to the standard questions are in `defense_numbers.md`; this page adds three
things: the order to run in, the hard follow-ups that sheet does not cover, and how to score the
rehearsal. The slides: "Thesis Defense: Quota and Rate Limiting", 21 slides, each with speaker notes.

## 1. The run sheet (about 40 minutes)

| Part | Time | What |
| --- | --- | --- |
| Talk | 18 min | Slides 1 to 21, about 50 s each (the timings below) |
| Demo 1 | 1.5 min | `scripts/live_demo.sh`. Backup: `dashboard/live_demo.png` |
| Demo 2 | 1.5 min | `KEEP=1 control/.venv/bin/python scripts/demo_v2.py`, then the portal. Backup: its printed output |
| Questions | 15 to 20 min | Section 2, in that order; the committee may skip around |
| Feedback | 5 min | Section 4 |

Slide timings, for the speaker:

| Slides | Section | Minutes |
| --- | --- | --- |
| 1 to 3 | The problem and the claim | 2.5 |
| 4 to 6 | The design: two paths and the lease | 3 |
| 7 to 11 | Evidence: exact, bounded, fast, robust, ablation | 5 |
| 12 | Scale (an estimate) | 1 |
| 13 to 17 | Many API owners: model, business, billing, capacity, G10 to G14 | 4.5 |
| 18 to 21 | Limits, two demos, summary | 2 + demos |

**Before you start:**
- Docker Desktop is running, and the dev cluster is up: `docker compose -p ratelimiter-dev -f docker/docker-compose.dev.yml up -d`.
- `build/release` is built.
- Both demo scripts have been run once that day.

## 2. The questions, in the order a committee tends to ask

1. **Positioning:** why not AWS API Gateway, Kong or Envoy? Who would use this? (sheet)
2. **The core claim:** how do you prove the quota is exact? Midnight with two instances at once? A retried
   request? (sheet)
3. **The bound:** can the lease admit more than N × C? (sheet) Then the follow-ups F1 to F3 below.
4. **Speed:** where does the time go? Was the load generator the bottleneck? (sheet)
5. **Failures:** a Redis master dies? Could retries make an outage worse? (sheet)
6. **Scale:** one laptop cannot do 1M/s, so is the claim false? Why only one machine? (sheet) Then F4.
7. **Phase 2:** can one merchant see or slow another? Why trust the invoice? A customer upgrades
   mid-month? A payment fails? (sheet) Then F5 and F6.
8. **Security:** how do you know it is secure? Why no client certificates? (sheet) Then F7.
9. **Scope and contribution:** F8 to F10.

## 3. Hard follow-ups (not in the sheet)

| # | Question | A short answer |
| --- | --- | --- |
| F1 | Why a Lua script and not `MULTI`/`EXEC` or `WATCH`? | A script reads, decides and writes in one atomic step on the server, in one round trip. `WATCH` retries on contention, which a hot customer would cause on every call. |
| F2 | What is C, and who chooses it? | The lease chunk: at most 10% of the limit by default (`lease_chunk_pct`), adapting to demand. A smaller C tightens the bound and costs more refills; the bound was measured at caps of 1%, 5% and 10% (34.7, 189.0 and 381.4 tokens against 40, 200 and 400). |
| F3 | Why is the quota not leased too? | It is billed, so it must be exact: one atomic script a check. Only large plain daily plans lease it, within their own bound, and monthly, boosted or topped-up plans never do, because only the script sees the Redis clock. |
| F4 | How do you know instances scale linearly? | I don't yet; it is labelled as an assumption. The AWS kit (`aws/README.md`) measures 1, 2 and 4 limiters on separate machines, for about 2 USD. |
| F5 | What if Redis loses the counter in a failover? | The last unreplicated increments can be lost; invoices then undercount, in the customer's favour. Three runs with a killed master lost nothing, and this is stated as a limit. Waiting for a replica (`WAIT`) would close the gap at a round trip per check. |
| F6 | Why per-merchant capacity after the burst check, not before? | So a call refused for capacity is never counted or billed. The noisy-merchant test found it then cost Redis for floods, so a known-empty capacity now refuses from the lease first. |
| F7 | What did the security reviews find? | Last time, 7 findings. The worst: a customer key starting with `}` split its Redis keys across masters, so a paid plan could be used uncounted. Keys with `{` or `}` are now refused, with a test that fails without the fix. |
| F8 | Phase 2 doubled the scope: was it needed? | The thesis is about selling access by plan. Without merchants, billing and capacity, it answered only "how to check a quota", not "how to run it as a service". Phase 1's claims did not change. |
| F9 | What is yours, and what is a library? | Libraries: gRPC, redis-plus-plus, FastAPI. Mine: the engines, the lease table, the scripts and their proof, the control plane, and every test and benchmark. |
| F10 | Three more months: what first? | Measure on separate machines; lease the burst limit per customer (closing the noisy-neighbour gap); a real payment provider. |

## 4. Scoring the rehearsal

Tick each one during the run; anything left unticked is what to practise.

- [ ] The talk ends within 20 minutes, demos included.
- [ ] The claim slide is said in one breath, without reading it.
- [ ] Every number comes with where it was measured ("on the laptop", "an estimate").
- [ ] "Estimated" is never said as "measured" (1M checks/s; scaling).
- [ ] Each limit is raised before the committee raises it (the "weak points" in the sheet).
- [ ] An answer starts with yes or no, then one reason, then the evidence. At most 30 s, unless asked
      for more.
- [ ] A question you cannot answer gets "I did not measure that; here is how I would" rather than a guess.
- [ ] A demo that fails moves to its backup within 10 s.
- [ ] Slide 13 (merchants) explains in one sentence why Phase 2 exists.

## 5. Doing it with Claude

Paste this in a chat with the slides open: "Play a thesis committee of three (a systems professor, an
industry engineer, a strict reviewer). Ask me the questions in docs/thesis/mock_defense.md section 2
and its follow-ups, one at a time; after each answer, give a one-line critique and a score out of 5.
At the end, list my three weakest answers." Run it twice: once with the sheet open, once without.
