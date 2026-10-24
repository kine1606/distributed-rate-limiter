#!/usr/bin/env python3
"""Chapter 5's latency table from bench/open_loop.sh: medians of the runs per cell, p99 with its range.

    tools/open_loop_table.py results/<date>_open_loop/summary.txt
    tools/open_loop_table.py --self-check
"""
import json
import statistics as st
import sys


def table(lines):
    cells = {}
    for line in lines:
        if line.startswith("{"):
            d = json.loads(line)
            cells.setdefault(d["cell"], []).append(d["result"])
    out = ["| Rule | Offered | p50 | p99 (min to max) | p999 | Failed or unanswered |", "| --- | --- | --- | --- | --- | --- |"]
    for cell, rs in cells.items():
        rule, rate = cell.rsplit("_", 1)
        p99 = [r["p99_ms"] for r in rs]
        bad = sum(r["errors"] + r["unanswered"] for r in rs)
        out.append(f"| `{rule}` | {int(rate):,}/s | {st.median(r['p50_ms'] for r in rs):.2f} ms | "
                   f"{st.median(p99):.2f} ms ({min(p99):.2f} to {max(p99):.2f}) | "
                   f"{st.median(r['p999_ms'] for r in rs):.2f} ms | {bad} |")
    return "\n".join(out) + "\n"


def self_check():
    runs = [{"cell": "api_default_10000", "result": {"p50_ms": p, "p99_ms": 2 * p, "p999_ms": 3 * p, "errors": 0,
                                                    "unanswered": 0}} for p in (0.1, 0.3, 0.2)]
    text = table(json.dumps(r) for r in runs)
    assert "| `api_default` | 10,000/s | 0.20 ms | 0.40 ms (0.20 to 0.60) | 0.60 ms | 0 |" in text, text
    print("open_loop_table self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        with open(sys.argv[1]) as f:
            sys.stdout.write(table(f))
