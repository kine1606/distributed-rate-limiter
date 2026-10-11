# Usage events

> Week 4 · Flow steps A13, B8 · Status: not started

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

## Done when

- [ ] Events written for every decision; drop counter exported.
- [ ] `usage_report.py` totals match the accuracy test's expected counts.
