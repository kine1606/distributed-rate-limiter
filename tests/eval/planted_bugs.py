#!/usr/bin/env python3
"""Plants one bug at a time in the engine, runs the property tests (tests/property_test.cc), and restores
the file from git. Run from a clean tree: tests/eval/planted_bugs.py
A mutation the tests do not catch is a gap in the tests."""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
MUTATIONS = [
    ("burst token not given back on a quota denial", "src/core/composite_core.cc",
     "if (!q.allowed) strict_.GiveBack(burst, plan, key, cost);", "if (false) strict_.GiveBack(burst, plan, key, cost);"),
    ("a replayed denial is cached locally", "src/core/composite_core.cc",
     "if (q.reason == Reason::kQuotaExhausted && !q.replayed) {", "if (q.reason == Reason::kQuotaExhausted) {"),
    ("burst checked before the deny cache", "src/core/composite_core.cc",
     "if (const std::uint32_t reset = deny_.DeniedFor(key, QuotaStamp(plan), now_()); reset > 0) {",
     "if (const std::uint32_t reset = deny_.DeniedFor(key, QuotaStamp(plan), now_()); reset > 0 && false) {"),
    ("a denial survives a change of the plan's quota terms", "src/lease/deny_cache.h",
     "if (it->second.until_ns <= now_ns || it->second.plan_stamp != plan_stamp) {", "if (it->second.until_ns <= now_ns) {"),
    ("a denial ends one second late", "src/lease/deny_cache.h",
     "if (it->second.until_ns <= now_ns ||", "if (it->second.until_ns + 1'000'000'000 <= now_ns ||"),
    ("an invalidated plan stays in the per-thread copies", "src/plan/plan_store.cc",
     "  if (it != s.map.end()) Expire(it->second);\n  generation_.fetch_add(1, std::memory_order_release);  // every thread's copies are stale now",
     "  if (it != s.map.end()) Expire(it->second);"),
    ("no stale plan during an outage", "src/plan/plan_store.cc",
     "} else if (it->second.has_plan) {", "} else if (false) {"),
    ("an outage on the burst check fails closed for every plan", "src/core/composite_core.cc",
     "if (RedisDown(b)) {  // no quota call: Redis is unlikely to answer it either\n        done(PlanDown(plan));",
     "if (RedisDown(b)) {  // no quota call: Redis is unlikely to answer it either\n        done(PlanDown(Plan{}));"),
    ("a leased quota chunk may grow to twice the cap", "src/lease/chunk_policy.h",
     "return std::clamp(next, ChunkMin(r), ChunkCap(r));", "return std::clamp(next, ChunkMin(r), 2 * ChunkCap(r));"),
    ("a lease refill adds a chunk instead of topping up", "src/core/leased_core.cc",
     "Refill(l, static_cast<std::uint32_t>(std::max<std::int64_t>(1, target - have)), 0);",
     "Refill(l, static_cast<std::uint32_t>(std::max<std::int64_t>(1, target)), 0);"),
]


def run(cmd):
    return subprocess.run(cmd, cwd=ROOT, shell=True, capture_output=True, text=True)


results = []
for name, path, old, new in MUTATIONS:
    f = ROOT / path
    text = f.read_text()
    if text.count(old) != 1:
        results.append((name, "NOT APPLIED (text not found)"))
        continue
    f.write_text(text.replace(old, new))
    try:
        b = run("cmake --build build/release --target rl_unit_tests 2>&1 | tail -3")
        if "error" in b.stdout:
            results.append((name, "BUILD FAILED: " + b.stdout[-300:]))
            continue
        t = run("RL_PROPERTY_SEEDS=300 build/release/tests/rl_unit_tests --gtest_filter='Property.*' 2>&1")
        failed = [l for l in t.stdout.splitlines() if l.startswith("[  FAILED  ] Property.")]
        first = next((l for l in t.stdout.splitlines() if " seed " in l and ": " in l and "replay" not in l), "")
        results.append((name, f"caught by {len(failed)} test(s): {first.strip()[:160]}" if failed else "NOT CAUGHT"))
    finally:
        run(f"git checkout -- {path}")
    print(results[-1], flush=True)

run("cmake --build build/release --target rl_unit_tests")
print("\nsummary:")
for name, outcome in results:
    print(f"- {name}: {outcome}")
sys.exit(0)
