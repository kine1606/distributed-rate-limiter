# dashboard

A static page that shows how far the project is: the roadmap week by week, the latest gate, benchmark
charts, the lease harness, test counts and recent commits. No server, libraries or network needed.

```sh
python3 tools/build_dashboard.py     # writes dashboard/data.js from the repo
```

Then open `dashboard/index.html` in a browser (from Windows: `D:\GitHub\distributed-rate-limiter\dashboard\index.html`).

Where the data comes from:

- `docs/ROADMAP.md`: weeks, checklist items and gates. A week's gate status is its `Gate status: met.`
  line (`met`, `partly met` or `not met`); a week without one shows as not started.
- `results/`: the latest `*_week3_gate/summary.txt`, `*_baseline_ramp_strict_c*.txt`,
  `*_lease_microbench/before.json` and `*_lease_harness.txt`. `results/` is git-ignored, so a fresh
  clone shows these sections as "not measured yet" until the benchmarks are run there.
- `ctest -N -L <label>` in `build/release` for test counts, and `git log` for commits.

`data.js` is a script rather than JSON so the page also works when opened straight from disk.

## Live view (while a test runs)

`scripts/live_demo.sh` starts a limiter and plays a 90-second story against it: normal traffic, one
customer flooding (refused by its burst, then out of quota and refused by the deny cache), an upgrade
with `tools/rlctl`, and one Redis master stalled for 4 s. Watch it at http://127.0.0.1:8090/
(opened for you on Windows). `live_demo.png` is a recorded run of it, the backup if a live demo fails.

To watch any running limiter instead, start it with metrics on (`RL_METRICS_ADDR`, default
127.0.0.1:9100) and run `tools/live_dashboard.py`. It serves `live.html` and relays `/metrics` from
the same origin, so the limiter needs no CORS. Charts: checks per second by decision (allowed, rate
limited, quota exhausted, Redis unreachable), and p50/p99 latency, over the last two minutes.
