# Thesis plan (Oct 5 - Nov 20, 2026)

The full plan (scope decisions, week-by-week tasks, thesis chapters, risks, milestones) is in the
shared planning doc. Weekly gates:

| Week | Dates | Focus | Gate |
| --- | --- | --- | --- |
| 1 | Oct 5-11 | Toolchain, Lua scripts, Redis Cluster compose | Both scripts pass edge-case tests |
| 2 | Oct 12-18 | gRPC server, strict baseline | Baseline numbers saved |
| 3 | Oct 19-25 | Lease fast path, batching | Lease throughput clearly above baseline |
| 4 | Oct 26-Nov 1 | Reliability, metrics, hot reload | Kill Redis, see documented behavior |
| 5 | Nov 2-8 | TSan/ASan, accuracy test, fault tests | All tests green; feature freeze Nov 8 |
| 6 | Nov 9-15 | Final benchmarks, thesis chapters 4-6 | Full draft |
| 7 | Nov 16-20 | Buffer, advisor feedback, slides | Delivered |
