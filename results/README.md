# Results

Raw benchmark and evaluation output lives here (git-ignored except this file). Chapter 5 tables
and figures are generated from these files by scripts in `tools/plots/`, never typed by hand.

## Naming

```text
YYYY-MM-DD_<test>_<engine>_<instances>inst_<extra>.json
2026-10-24_baseline_strict_1inst_c64.json
2026-11-25_matrix_leased_4inst_chunk5pct_zipf.json
```

## Next to every result file

Write a `.md` file with the same name containing: the exact command, git commit hash, hardware,
WSL and Docker versions, rule config, plan presets, and anything unusual (thermal throttling,
background load). Five minutes now saves a day in December, when you no longer remember which run
used which chunk size.
