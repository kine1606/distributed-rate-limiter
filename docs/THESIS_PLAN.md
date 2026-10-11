# Thesis plan (Oct 12 to Dec 15, 2026)

The full plan (scope, risks, evaluation design) is in the shared planning doc. The day-to-day
checklist with links to every module is [ROADMAP.md](ROADMAP.md).

| Week | Dates | Focus | Gate |
| --- | --- | --- | --- |
| 1 | Oct 12-18 | Redis client spike, cluster, Lua scripts, proto v1 | Both scripts pass every edge case; proto v1 agreed |
| 2 | Oct 19-25 | Strict engine end to end, baseline | Free customer stopped at exactly 200 via gRPC; baseline saved |
| 3 | Oct 26-Nov 1 | Lease fast path, batching | Lease mode clearly faster than strict |
| 4 | Nov 2-8 | Overage, request ID, deny cache, usage events | Request 2,001 gives three outcomes on three policies |
| 5 | Nov 9-15 | Reliability, metrics, Chapter 3 | Kill Redis, observe documented behaviour |
| 6 | Nov 16-22 | TSan/ASan, evaluation tests, freeze Nov 22 | All tests green |
| 7 | Nov 23-29 | Benchmark matrix, profile, Chapter 4 | Matrix and flamegraph saved |
| 8 | Nov 30-Dec 6 | Final numbers, Chapter 5 | Full draft by Dec 6 |
| 9 | Dec 7-15 | Advisor feedback, slides, demo | Submit Dec 15 |
