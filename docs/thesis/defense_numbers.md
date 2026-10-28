# Defense sheet: final numbers and evidence (2026-10-24, merchants added 2026-10-28)

This sheet updates `Thesis_Defense_Brief.docx`: its "numbers to know by heart" with the final values,
and each of its questions with the measurement that answers it. Every number has a source in
`docs/thesis/evidence/` or the code.

## Numbers to know by heart

| Number | What it is | Status and source |
| --- | --- | --- |
| 1,000,000 checks/s | cluster-wide goal | **estimated, never measured**: 15 to 50 leased instances, 17 masters for customer checks, about $18,500 a month (`tools/extrapolate.py`, 5.11) |
| p50 < 500 µs, p99 < 2 ms | latency goal (G6) | leased rules: met up to 20,000/s per instance (p50 0.43, p99 1.45 ms); customer check: p99 1.88 ms at 5,000/s, p50 0.94 ms not met (`open_loop_2026-10-24.md`) |
| 200 / 2,000 a day | Free and Pay quotas | design (`tools/plans.sh`) |
| 2 and 20 tokens/s | Free and Pay burst limits, burst 2 and 20 | design |
| 10% grace (200), cap +1,000 | overage of the grace and cap plans | design |
| 00:00 UTC by Redis's clock | daily reset | design; tested end to end on two instances |
| N × C | proved bound on burst over-admission | measured 34.7 / 189.0 / 381.4 against 40 / 200 / 400 (`burst_bound_2026-10-22.md`) |
| 10% | default chunk cap (`lease_chunk_pct`) | a bigger cap is a little faster where keys repeat (matrix) and looser (bound) |
| off | the batcher | **changed from the brief**: measured, no gain; the 200 µs timer cost 130 µs at p50 (`results/2026-10-18_batcher`) |
| 50 ms | deadline of each Redis attempt | **changed from the brief's 5 to 10 ms**: the client's own timeout is per idle connection, so each attempt has its own deadline |
| 20 failures, 500 ms | breaker: opens after 20 in a row, probes after 500 ms | one breaker per 256 of the 16,384 slots |
| 16,384 | Redis Cluster hash slots | a customer's keys share one slot by hash tag |
| 3 masters | test cluster | 3 + 3 replicas for fault tests |
| 64 bytes | lease alignment | 14 times faster per-thread counters at 16 threads (microbenchmark); within noise in the whole server (ablation) |
| 279 tests | release, ASan, TSan | 216 unit, 42 against a real Redis, 21 integration; plus 39 of the control plane (Python); 97.1% line coverage |
| 42,065 checks/s | one instance, strict, uniform keys | matrix, pinned (Week 2's ghz baseline: 25,301) |
| 67,459 / 95,913 / 400,187 checks/s | one instance, leased: Zipf keys / one hot key / `CheckBatch` of 10 | matrix and its CheckBatch section |
| 82,700 to 91,200 checks/s | gRPC alone, one instance | the ceiling of any limiter on this laptop |
| 99.06% | lease hit ratio, Week 2 load | `results/2026-10-18_week3_gate` |
| about 120,000/s | script calls one master absorbs | Week 1 spike (360,000 on 3 masters); FCALL = EVALSHA |
| 4 to 6% | the limiter's own code in the CPU profile | gRPC 40 to 53%, allocation 16 to 20% (`profile_2026-10-24.md`) |

## Merchants (Phase 2): numbers to know

Evidence: `p4_merchants_2026-10-28.md` and `p3_gate_2026-10-25.md`; Chapter 5.12.

| Number | What it is | Source |
| --- | --- | --- |
| `<merchant>\|<customer>` | how merchants are kept apart: every key, lease and denial below the service | 3.9.2 |
| 9 of 9 | isolation checks; a planted shared namespace fails 4 | 5.12 |
| 10,135 → 30,144 Redis calls/s | another merchant's flood over one customer's burst (the open gap); with the operator's check rate: 10,281 | Table 5.14 |
| 42,000 → 12,400 Redis calls/s | a flood over a merchant's capacity, before and after the early refusal found by this test | Table 5.15 |
| 6.6 ms | a change (suspend, quota, boost, capacity) in force on both of two limiters after the API answered; 16 ms with the API call (G11: 1 s) | Table 5.16 |
| 0 customers | with an invoice different from the counters, in 3 runs, 2 with a master killed (G12) | Table 5.17 |
| seed 2 | where a planted bug letting overage pass the cap by one call is caught (G13) | 5.12 |
| 1,003 of 1,000/s | the most a capacity of 1,000 admitted; Free kept to 687 of its 700; paying customers kept the reserve (G14) | Table 5.18 |
| 193 bytes | Redis memory per customer, 10,000 merchants of 100 customers | 5.12 |
| 25,749 calls/s | one limiter, a random merchant and customer per call, a million customers | 5.12 |
| 37 ms | a new merchant's key working after it is announced, with 10,000 loaded | 5.12 |
| +2.1%, −3.9% | tenancy, and tenancy with a capacity and check rate, against none: within the ±10% noise | Table 5.19 |

## The brief's questions, with the evidence

| Question | Answer in one line | Evidence |
| --- | --- | --- |
| Why not AWS API Gateway, Kong or Envoy? | Their limits are per key and approximate; none bills daily quotas exactly with request ids and overage. Envoy's service was measured on the same laptop | `ceiling_and_envoy_2026-10-19.md`, Chapter 2 |
| Who would use this? | Sellers of API access by plan: AI models, SMS and OTP, maps and data | Chapter 1 |
| How do you prove the quota is exact? | Atomic scripts; 20 of 20 runs on four instances exact; 22.6 M checks equal to a model; 10 of 10 planted bugs caught | 5.2 |
| Midnight with two instances at once? | One script resets the counter by the master's clock; the new end-to-end test sees exactly one reset | 5.3, `plan_change_it.cc` |
| Upgrade while requests are in flight? | Applied on the next call on every instance, 22 ms after the admin tool; usage kept | 5.3 |
| A retried request that succeeded? | Same request id within 5 minutes: the stored answer, not counted again; 9,608 duplicates counted once | 5.2 |
| Can the lease admit more than the bound? | No: proved, 100,000 seeded schedules, and on the cluster 381.4 against 400. A refill tops up to one chunk, never adds one; that fix came from the harness | 5.4, 3.5 |
| One laptop cannot do 1M/s: is the claim false? | The claim is an estimate with every input measured and every assumption written; it is labelled so | 5.11 |
| Where does the time go in one check? | gRPC 40 to 53% of the CPU, allocation 16 to 20%, the limiter 4 to 6%; a leased check p50 0.21 ms at 10,000/s | 5.10 |
| Why p99 over the average? | A gateway call waits for the slowest check it makes; tails decide timeouts. The latencies are measured open loop, from each call's scheduled time | 5.6 |
| Was the load generator the bottleneck? | ghz was (Week 3), so a C++ generator replaced it. It is pinned to other cores; Little's law holds in all 30 cells; send lag p99 at most 0.51 ms | 5.6 |
| A Redis master dies? | Only its third of the customers get their plan's policy for 4 to 5 s; no call fails | 5.7 |
| Could retries make an outage worse? | Retries only on timeouts, never for quota, at most two, jittered, and counted by the breaker, which stops calls after 20 failures | 3.7, 4.6 |
| Leased tokens when an instance restarts? | A clean stop returns them (`return_lease.lua`); a crash loses them, which is stricter, never looser | 3.5, `DESIGN.md` 8 |
| Why only one machine? | No budget for a cluster; the extrapolation names what a run on separate machines would test | 5.13, 6.3 |
| Why drop the sliding window? | **It was not dropped:** `lua/sliding_window.lua` (a sliding-window counter, O(1) state) serves `login_attempts`, exactly | `config/rules.yaml` |
| What next? | Lease the burst per customer, route customers to one instance, batch at the gateway, multi-region | 6.3 |
| Can one merchant see or slow another? | No key, plan, lease or denial is shared: the service names every customer `<merchant>\|<customer>` from the API key. Floods refused by the merchant's own limits cost Redis almost nothing; a burst flood needs the operator's check rate | 3.9.2, 5.12 |
| Why trust the invoice? | It is made from totals the quota script keeps in the same atomic step as each decision; equal to the counters with a master killed, in every run | 3.9.6, 5.12 |
| What if a paying customer has an emergency? | Postpaid: past its quota it is admitted as billed overage up to a spending cap (its burst limit and the merchant's capacity still apply); or a boost, or a top-up | 3.9.4 |
| What stops a merchant's backend being flooded? | Its capacity, a leased bucket; a reserve for plans with a fee; Free calls refused first | 3.9.7, 5.12 |
| Are card numbers stored? | No: the API refuses them; only the payment provider's customer id is kept | 3.9.6 |

## Weak points to raise first

- **No measurement on separate machines.** Every scaling statement is an estimate.
- **The customer check misses the p50 goal** (0.94 ms against 0.5) because of two Redis round trips.
  Leasing the burst per customer is the fix.
- **One run allowed about 30,000 calls uncounted** while a paid customer crossed its quota, most
  likely fail-open during a Redis stall that was not observed. Eleven repeats were exact, and the
  load generator now counts such answers.
- **The Universal Scalability Law did not fit one instance's curve.** The data shows instead that
  throughput never falls with concurrency.
- **A merchant's flood over one customer's burst still costs Redis** (30,144 calls/s) and slowed
  another merchant, until the operator set a check rate. The design leaves that to the operator.
- **The reserve is a floor, not a priority:** paying customers asking for more than the reserve compete
  with Free calls for the rest.
- **Billing through failover:** three runs lost nothing, but asynchronous replication can lose an
  acknowledged increment; invoices would then undercount, in the customer's favour.
