#!/usr/bin/env python3
"""Chapter 5 tables from the benchmark matrix (bench/run_matrix.sh): every number in them comes from
results/<date>_matrix/summary.txt, as medians of the runs with the min-max range.

    tools/matrix_tables.py results/<date>_matrix/summary.txt     prints the tables as Markdown
    tools/matrix_tables.py --self-check
"""
import json
import statistics
import sys
from collections import defaultdict

ENGINES = [("allow_all", None, "gRPC alone (`allow_all`)"), ("strict", None, "strict (Redis per check)"),
           ("leased", 1, "leased, cap 1%"), ("leased", 5, "leased, cap 5%"), ("leased", 10, "leased, cap 10%"),
           ("customer", None, "customer check (burst + quota)")]
DISTS = [("uniform", "uniform, 1M keys"), ("zipf", "Zipf 0.99, 1M keys"), ("hot", "one hot key")]


def load(lines):
    cells = defaultdict(list)
    for line in lines:
        if line.startswith("{"):
            d = json.loads(line)
            cells[(d["engine"], d.get("chunk_pct"), d["instances"], d["dist"])].append(d)
    return cells


def thousands(x):
    return f"{x:,.0f}"


def cell_text(runs):
    if not runs:
        return "-"
    c = [r["checks_per_s"] for r in runs]
    return f"{thousands(statistics.median(c))} ({thousands(min(c))} to {thousands(max(c))})"


def tables(cells):
    out = []
    out.append("Checks per second, by engine and number of instances (uniform keys over 1M; median of runs, range):\n")
    out.append("| Engine | 1 instance | 2 instances | 4 instances | p99, 1 instance | Allowed, 1 instance |")
    out.append("| --- | --- | --- | --- | --- | --- |")
    for engine, cap, name in ENGINES:
        row = [cell_text(cells.get((engine, cap, n, "uniform"), [])) for n in (1, 2, 4)]
        one = cells.get((engine, cap, 1, "uniform"), [])
        p99 = f"{statistics.median(r['p99_ms'] for r in one):.2f} ms" if one else "-"
        allowed = (f"{100 * statistics.median(r['allowed_per_s'] / max(1, r['checks_per_s']) for r in one):.0f}%"
                   if one else "-")
        out.append(f"| {name} | " + " | ".join(row) + f" | {p99} | {allowed} |")
    out.append("")
    out.append("Checks per second on 1 instance, by key distribution:\n")
    out.append("| Engine | " + " | ".join(d for _, d in DISTS) + " |")
    out.append("| --- |" + " --- |" * len(DISTS))
    for engine, cap, name in ENGINES:
        if engine == "allow_all":
            continue
        out.append(f"| {name} | " + " | ".join(cell_text(cells.get((engine, cap, 1, d), [])) for d, _ in DISTS) + " |")
    out.append("")
    return "\n".join(out) + "\n"


def self_check():
    runs = [{"engine": "leased", "chunk_pct": 10, "instances": 1, "dist": "uniform", "checks_per_s": v,
             "allowed_per_s": v, "p99_ms": 4.0, "errors": 0} for v in (100, 300, 200)]
    text = tables(load(json.dumps(r) for r in runs))
    assert "| leased, cap 10% | 200 (100 to 300) | - | - | 4.00 ms | 100% |" in text, text
    assert "| strict (Redis per check) | - | - | - | - | - |" in text, text
    print("matrix_tables self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        with open(sys.argv[1]) as f:
            sys.stdout.write(tables(load(f)))
