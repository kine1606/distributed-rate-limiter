#!/usr/bin/env python3
"""Summarises gcov coverage of the project's own code (src/, include/) after the tests ran in a
--coverage build (scripts/coverage.sh).

    tools/coverage_report.py <build dir>     prints a table per file, then each file's uncovered lines
    tools/coverage_report.py --self-check

A line counts as covered if any test executed it in any translation unit; a header line is merged
over every unit that includes it. Branches are gcov's (each outcome of a condition), so they include
those the compiler adds; read them as a pointer to code to look at, not as a target.
"""
import gzip
import json
import os
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OWN = ("src/", "include/")


def merge(reports):
    """{file: {line: (count, [branch counts])}} over gcov JSON reports, counts summed per line."""
    files = defaultdict(dict)
    for report in reports:
        for f in report.get("files", []):
            path = os.path.relpath(os.path.normpath(os.path.join(report.get("current_working_directory", ""), f["file"])), ROOT)
            if not path.startswith(OWN) or "/spike/" in path:
                continue
            lines = files[path]
            for ln in f["lines"]:
                n = ln["line_number"]
                count, branches = lines.get(n, (0, []))
                b = [br["count"] for br in ln.get("branches", [])]
                if branches and len(branches) == len(b):
                    b = [x + y for x, y in zip(branches, b)]
                elif branches and len(branches) > len(b):
                    b = branches
                lines[n] = (count + ln["count"], b)
    return files


def gcov_reports(build):
    gcdas = []
    for dirpath, _, names in os.walk(os.path.abspath(build)):
        gcdas += [os.path.join(dirpath, n) for n in names if n.endswith(".gcda")]
    reports = []
    for gcda in gcdas:
        out = subprocess.run(["gcov", "--json-format", "--stdout", "--branch-probabilities", gcda],
                             cwd=os.path.dirname(gcda), capture_output=True, text=True)
        for line in out.stdout.splitlines():
            if line.startswith("{"):
                reports.append(json.loads(line))
    return reports


def summarise(files):
    rows, details = [], []
    total_l = total_lc = total_b = total_bc = 0
    for path in sorted(files):
        lines = files[path]
        l = len(lines)
        lc = sum(1 for c, _ in lines.values() if c > 0)
        b = sum(len(br) for _, br in lines.values())
        bc = sum(1 for _, br in lines.values() for x in br if x > 0)
        total_l, total_lc, total_b, total_bc = total_l + l, total_lc + lc, total_b + b, total_bc + bc
        rows.append(f"{path:48s} lines {lc:5d}/{l:<5d} {100 * lc / l if l else 100:5.1f}%   branches {bc:5d}/{b:<5d} {100 * bc / b if b else 100:5.1f}%")
        missed = sorted(n for n, (c, _) in lines.items() if c == 0)
        if missed:
            details.append(f"{path}: uncovered lines {compact(missed)}")
    head = (f"{'TOTAL':48s} lines {total_lc:5d}/{total_l:<5d} {100 * total_lc / max(1, total_l):5.1f}%   "
            f"branches {total_bc:5d}/{total_b:<5d} {100 * total_bc / max(1, total_b):5.1f}%")
    return "\n".join([head] + rows + [""] + details) + "\n"


def compact(nums):
    """[1, 2, 3, 7] -> '1-3, 7'"""
    out, start = [], None
    for i, n in enumerate(nums):
        if start is None:
            start = n
        if i + 1 == len(nums) or nums[i + 1] != n + 1:
            out.append(f"{start}-{n}" if n != start else str(n))
            start = None
    return ", ".join(out)


def self_check():
    assert compact([1, 2, 3, 7, 9, 10]) == "1-3, 7, 9-10"
    report = {"current_working_directory": ROOT, "files": [
        {"file": "src/a.cc", "lines": [{"line_number": 1, "count": 0, "branches": [{"count": 0}, {"count": 2}]},
                                       {"line_number": 2, "count": 3, "branches": []}]}]}
    files = merge([report, report])
    assert files["src/a.cc"][1] == (0, [0, 4]) and files["src/a.cc"][2] == (6, [])
    text = summarise(files)
    assert "lines     1/2" in text and "uncovered lines 1" in text, text
    print("coverage_report self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        sys.stdout.write(summarise(merge(gcov_reports(sys.argv[1]))))
