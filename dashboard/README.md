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
