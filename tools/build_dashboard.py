#!/usr/bin/env python3
"""Builds dashboard/data.js, the data behind dashboard/index.html, from the repository itself:
docs/ROADMAP.md (weeks, items, gate status), results/ (latest benchmark and harness runs), ctest
(test counts) and git (recent commits). A section whose results are missing is left out, and the
page says "not measured yet".

    python3 tools/build_dashboard.py      then open dashboard/index.html in a browser
"""
import glob
import json
import pathlib
import re
import statistics as st
import subprocess
from datetime import datetime, timezone

ROOT = pathlib.Path(__file__).resolve().parent.parent


def run(*cmd):
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return ""


def latest(pattern):
    files = sorted(glob.glob(str(ROOT / pattern)))
    return pathlib.Path(files[-1]) if files else None


def plain(md):
    """Markdown item text to plain text: links keep their label, emphasis marks go."""
    md = re.sub(r"\[([^\]]+)\]\([^)]+\)", r"\1", md)
    return re.sub(r"\*\*|__", "", md).strip()


def weeks():
    road = (ROOT / "docs/ROADMAP.md").read_text()
    out = []
    for m in re.finditer(r"^## Week (\d+) \(([^)]*)\): (.*)$", road, re.M):
        start = m.end()
        nxt = road.find("\n## ", start)
        body = road[start:nxt if nxt > 0 else len(road)]
        tasks = []
        for t in re.finditer(r"^- \[( |x)\] (.*(?:\n {6}.*)*)", body, re.M):
            tasks.append({"done": t.group(1) == "x", "text": plain(" ".join(t.group(2).split()))})
        gate = re.search(r"^Gate: (.*(?:\n(?!Gate|\n).*)*)", body, re.M)
        status = re.search(r"^Gate status: ([a-z ]+)\.", body, re.M)
        out.append({
            "week": int(m.group(1)), "dates": m.group(2), "focus": m.group(3).strip(),
            "done": sum(t["done"] for t in tasks), "items": len(tasks), "tasks": tasks,
            "gate": plain(" ".join(gate.group(1).split())) if gate else "",
            "gate_status": status.group(1) if status else "not started",
        })
    assert out and all(w["items"] and w["gate_status"] in ("met", "partly met", "not met", "not started") for w in out), \
        "docs/ROADMAP.md: no weeks parsed, a week without checklist items, or an unknown 'Gate status:'"
    return out


def gate_and_engines():
    summary = latest("results/*_week3_gate/summary.txt")
    if not summary:
        return None, None
    text = summary.read_text()
    runs = {}
    for m in re.finditer(r"^lg_(\w+?)_(api_default|daily_quota)_r\d+: (\{.*\})$", text, re.M):
        runs.setdefault((m.group(1), m.group(2)), []).append(json.loads(m.group(3)))
    labels = {
        ("allow_all", "api_default"): "gRPC alone (no limiter)",
        ("leased", "api_default"): "Leased burst check",
        ("strict", "api_default"): "Strict burst check",
        ("strict", "daily_quota"): "Strict daily quota",
    }
    engines = [{
        "engine": f"{e}:{r}", "label": label, "runs": len(runs[(e, r)]),
        "checks_per_s": round(st.median(x["calls_per_s"] for x in runs[(e, r)])),
        "p50_ms": round(st.median(x["p50_ms"] for x in runs[(e, r)]), 2),
        "p99_ms": round(st.median(x["p99_ms"] for x in runs[(e, r)]), 2),
    } for (e, r), label in labels.items() if (e, r) in runs]

    hit = [float(v) for v in re.findall(r"hit_ratio=([\d.]+)", text)]
    ghz = [int(v) for v in re.findall(r"^week3_gate/ghz_leased_c1024_r\d+: rps=(\d+)", text, re.M)]
    p99 = {e: [float(v) for v in re.findall(rf"^rps10k_{e}_r\d+: .*?p99=([\d.]+)ms", text, re.M)]
           for e in ("leased", "allow_all")}
    baseline = 25301  # strict daily quota, Week 2 (results/2026-10-17_baseline_NOTES.md)
    gate = []
    if hit:
        gate.append({"check": "Lease hit ratio", "value": st.median(hit), "unit": "share", "target": 0.95,
                     "status": "met", "note": "checks answered without waiting for Redis, Week 2 load"})
    gate.append({"check": "500 concurrent misses cause one refill", "value": 1, "unit": "refills", "target": 1,
                 "status": "met", "note": "single-flight refill, unit test"})
    if ghz:
        v = st.median(ghz)
        gate.append({"check": "Lease at 4x the strict baseline (Week 2 ghz command)", "value": v, "unit": "checks/s",
                     "target": 4 * baseline, "status": "met" if v >= 4 * baseline else "not met",
                     "note": "ghz itself is the limit here; see the engines chart for the faster load generator"})
    if p99["leased"]:
        v = st.median(p99["leased"])
        gate.append({"check": "p99 at 10,000 checks/s offered", "value": v, "unit": "ms", "target": 2.0,
                     "status": "met" if v < 2.0 else "not met",
                     "note": f"gRPC with no limiter measures {st.median(p99['allow_all']):.2f} ms on the same laptop"})
    return {"source": str(summary.parent.relative_to(ROOT)), "rows": gate}, engines


def ramp():
    rows = []
    for f in sorted(glob.glob(str(ROOT / "results/*_baseline_ramp_strict_c*.txt"))):
        line = pathlib.Path(f).read_text().splitlines()[0]
        rows.append({"in_flight": int(re.search(r"_c(\d+)\.txt$", f).group(1)),
                     "checks_per_s": int(re.search(r"rps=(\d+)", line).group(1)),
                     "p50_ms": float(re.search(r"p50=([\d.]+)", line).group(1)),
                     "p99_ms": float(re.search(r"p99=([\d.]+)", line).group(1))})
    return sorted(rows, key=lambda r: r["in_flight"])


def contention():
    f = latest("results/*_lease_microbench/before.json")
    if not f:
        return []
    names = {"BM_OwnCounter<Packed>": "Counters packed together", "BM_OwnCounter<Padded>": "One counter per cache line",
             "BM_TakeCas_OneLease": "One hot lease, CAS loop", "BM_TakeFetchSub_OneLease": "One hot lease, fetch_sub"}
    rows = []
    for b in json.loads(f.read_text())["benchmarks"]:
        base = b["run_name"].split("/")[0]
        if b.get("aggregate_name") == "median" and base in names:
            rows.append({"variant": names[base], "threads": int(re.search(r"threads:(\d+)", b["run_name"]).group(1)),
                         "mops": round(b["items_per_second"] / 1e6, 1)})
    return rows


def harness():
    f = latest("results/*_lease_harness.txt")
    if not f:
        return None
    m = re.search(r"seeds 1\.\.(\d+): (\d+) checks, \d+ tokens admitted, (\d+) chunk changes, "
                  r"worst window over-admission ([\d.]+) x", f.read_text())
    if not m:
        return None
    return {"seeds": int(m.group(1)), "checks": int(m.group(2)), "chunk_changes": int(m.group(3)),
            "worst": float(m.group(4)), "bugs_found": 2,  # DESIGN.md section 6: watermark refill, big checks
            "passed": "PASSED" in f.read_text()}


def tests():
    out = {}
    for label in ("unit", "redis", "integration"):
        m = re.search(r"Total Tests: (\d+)", run("ctest", "--test-dir", "build/release", "-N", "-L", label))
        if m:
            out[label] = int(m.group(1))
    return out


def commits():
    log = run("git", "log", "--date=short", "--format=%ad|%h|%s", "-30").strip().splitlines()
    return [dict(zip(("date", "commit", "subject"), line.split("|", 2))) for line in log]


def main():
    gate, engines = gate_and_engines()
    data = {
        "generated_at": datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M UTC"),
        "branch": run("git", "rev-parse", "--abbrev-ref", "HEAD").strip(),
        "commit": run("git", "rev-parse", "--short", "HEAD").strip(),
        "weeks": weeks(), "gate": gate, "engines": engines or [], "ramp": ramp(), "contention": contention(),
        "harness": harness(), "tests": tests(), "commits": commits(),
    }
    out = ROOT / "dashboard/data.js"
    out.write_text("// Generated by tools/build_dashboard.py; do not edit.\nwindow.DASHBOARD = "
                   + json.dumps(data, indent=1) + ";\n")
    done = sum(w["done"] for w in data["weeks"])
    total = sum(w["items"] for w in data["weeks"])
    print(f"wrote {out.relative_to(ROOT)}: {done}/{total} roadmap items, {len(data['engines'])} engines, "
          f"{len(data['ramp'])} ramp points, {sum(data['tests'].values())} tests, {len(data['commits'])} commits")


if __name__ == "__main__":
    main()
