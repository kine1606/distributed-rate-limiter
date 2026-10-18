# Usage events

> Week 4 · Flow steps A13, B8 · Status: Week 4 done: `usage_log.{h,cc}`, `tools/usage_report.py`; `RL_USAGE_FILE=<file>` turns it on. A smoke run matched Redis `used` exactly (5,049 = 5,049, with 1,757 retried request IDs not counted again)

## What this module does

Redis counters answer "may this request pass right now?". Usage events answer "what happened?"
later: how many requests each customer made, how many were overage, how many were denied. Billing
(out of scope) would read them to build an invoice, and you read them in the evaluation to prove
there were no double counts. Redis counters expire every day; usage events are the durable record.

## Design in one paragraph

Each instance keeps a bounded in-memory buffer. The engine calls `Record()` once per decision,
which only appends to the buffer. A background thread aggregates events per customer per minute
and writes them to an append-only file (JSON lines) or a SQLite table every second. Aggregating per
minute keeps the output small: a million requests from one customer in a minute become one line.

## Skeleton

```cpp
struct UsageEvent {
  std::uint64_t customer_id;      // interned id, not a string: no allocation
  std::uint32_t minute;           // Redis time / 60000
  std::uint8_t outcome;           // allowed, overage, denied_burst, denied_quota, unverified
  std::uint32_t cost;
};

class UsageSink {
 public:
  virtual ~UsageSink() = default;
  virtual void Record(const UsageEvent& e) = 0;   // must not block or allocate
};

class FileUsageSink final : public UsageSink {
 public:
  FileUsageSink(std::string path, std::size_t capacity);
  void Record(const UsageEvent& e) override;      // push into a bounded queue; drop + count if full
  void Flush();                                    // called by the background thread and on shutdown
};
```

Output line example:

```json
{"instance":"limiter-1","customer":"cust:42","minute":29862720,"allowed":1180,"overage":12,"denied_burst":40,"denied_quota":0,"unverified":0}
```

## Build it in this order

1. Start with the simplest version that is correct: a mutex-protected vector, flushed every second.
   Measure its cost under load before replacing it with something lock-free.
2. Add aggregation per (customer, minute) at flush time, not at `Record` time, so the hot path
   stays a single append.
3. Drop events when the buffer is full and count drops in a metric. The response must never wait
   for usage recording.
4. Flush on shutdown as the last step before exit.
5. Write a small script (`tools/usage_report.py`) that sums the files of all instances per
   customer. The quota accuracy test compares this sum with the expected 200 and 2,000.

## Hints

- Mark `unverified` admissions (allowed by fail-open while Redis was down). The thesis can then
  report exactly how much traffic passed without a Redis check during the fault test.
- Request ID deduplication happens before `Record` is called, so a retried request produces one
  event. If you see more events than allowed requests in the accuracy test, deduplication is broken.
- A Redis Stream is a reasonable production alternative; mention it in Chapter 6 rather than
  building it.

## What was built (Week 4)

- `UsageLog` (`usage_log.h`) instead of the `UsageSink` interface: there is one sink, and
  `CompositeCore` takes a `UsageObserver` callback (like `LeasedCore`'s chunk observer), so tests
  record decisions without a file. Events are recorded for customer rules only: those are the
  ones a plan bills.
- One field per `Reason` (`allowed`, `rate_limited`, `quota_exhausted`, `unavailable`,
  `unverified`, `overage`, `suspended`), in cost units. `minute` is the wall-clock UTC minute of the
  instance, not Redis `TIME`: it only groups lines for reports.
- Lock-free (Week 4, after the mutex version was measured): 16 bounded rings of about 65,536
  events in all, each thread writing to one (Vyukov's queue: one CAS to claim a cell, a release
  store to publish it; the flush thread is the only reader). `Record` never allocates for keys of up
  to 15 bytes (the string's small-buffer size) and never waits for disk. The flush thread wakes
  every second, or early when a ring is half full; a full ring drops and counts (`Dropped()`, logged
  at shutdown; a metric in Week 5). A failed write counts as dropped. One customer on 16 threads:
  5.3 to 55.4 M kept events/s; the single flush thread is now the ceiling
  (`results/2026-10-18_customer_microbench/NOTES.md`).
- Aggregated at flush time, so a busy customer gets one line per minute per flush (up to 60 a
  minute); `tools/usage_report.py` sums them across files.
- Deduplication: `quota.lua` now returns a sixth value, `replayed`, and `CompositeCore` skips the
  stored answer of a retried request ID, so a retry produces one event (the first). The same flag keeps a
  replayed denial out of the deny cache.
- Customer IDs come from clients, so everything outside printable ASCII is escaped as `\u00XX`.

## Done when

- [x] Events written for every decision (`tests/usage_log_test.cc`,
      `CompositeCoreTest.EveryCustomerDecisionIsRecordedOnce`); drop counter kept, exported in Week 5.
- [x] `usage_report.py` totals match Redis `used` (smoke run on 2026-10-18); the accuracy test
      (Week 6) uses the same comparison.
