#!/usr/bin/env python3
"""Edge cases for the evaluation tools of Weeks 5 to 9 (2026-11-01): the tables, models and checks
the thesis numbers come from. Run by ctest (label unit), or: python3 -m unittest tests/tools_test.py
"""
import contextlib
import io
import json
import math
import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path[:0] = [os.path.join(ROOT, "tools"), os.path.join(ROOT, "tests", "eval")]

import bound_model as bm  # noqa: E402
import coverage_report  # noqa: E402
import extrapolate  # noqa: E402
import matrix_tables  # noqa: E402
import open_loop_table  # noqa: E402
import profile_summary  # noqa: E402
import usage_report  # noqa: E402

try:
    import usl_fit
except ImportError:  # numpy or scipy missing
    usl_fit = None


def read(path):
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        return f.read()


# ---- Week 5: the open-loop load generator's table ----

class Week5OpenLoopTable(unittest.TestCase):
    @staticmethod
    def run_line(cell, p50, errors=0, unanswered=0):
        return json.dumps({"cell": cell, "result": {"p50_ms": p50, "p99_ms": 2 * p50, "p999_ms": 3 * p50,
                                                    "errors": errors, "unanswered": unanswered}})

    def test_a_rule_name_with_underscores_keeps_them(self):
        text = open_loop_table.table([self.run_line("login_attempts_5000", 0.5)])
        self.assertIn("| `login_attempts` | 5,000/s |", text)

    def test_failed_and_unanswered_calls_are_summed_over_runs(self):
        text = open_loop_table.table([self.run_line("r_100", 1, errors=2, unanswered=3),
                                      self.run_line("r_100", 1, errors=0, unanswered=4)])
        self.assertTrue(text.rstrip().endswith("| 9 |"), text)

    def test_lines_that_are_not_json_are_skipped(self):
        text = open_loop_table.table(["loadgen 1.0 starting", "", self.run_line("r_100", 1)])
        self.assertEqual(text.count("| `r` |"), 1, text)

    def test_one_run_shows_its_p99_as_its_own_range(self):
        text = open_loop_table.table([self.run_line("r_100", 1.5)])
        self.assertIn("| 3.00 ms (3.00 to 3.00) |", text)


# ---- Week 6: the bound model (an independent simulation of the lease protocol) ----

class Week6BoundModel(unittest.TestCase):
    def test_a_missing_bucket_is_full(self):
        b = bm.Bucket(5, 1)
        self.assertTrue(b.take(0.0, 5)[0])
        self.assertFalse(b.take(0.0, 1)[0])

    def test_the_bucket_refills_by_whole_milliseconds_like_redis(self):
        b = bm.Bucket(10, 1000)  # one token per ms
        self.assertTrue(b.take(0.0, 10)[0])
        self.assertFalse(b.take(0.0009, 1)[0])  # 0.9 ms is 0 ms on the Redis clock
        self.assertTrue(b.take(0.001, 1)[0])

    def test_a_clock_that_steps_back_creates_no_tokens(self):  # lua/token_bucket.lua: elapsed < 0 is 0
        b = bm.Bucket(10, 100)
        self.assertTrue(b.take(1.0, 10)[0])
        self.assertFalse(b.take(0.5, 1)[0])

    def test_a_cost_above_the_capacity_is_denied_with_no_retry_time(self):
        self.assertEqual(bm.Bucket(5, 10).take(0.0, 6), (False, 5, 0))

    def test_a_bucket_that_never_refills_gives_no_retry_time(self):
        b = bm.Bucket(1, 0)
        self.assertTrue(b.take(0.0, 1)[0])
        self.assertEqual(b.take(100.0, 1), (False, 0, 0))

    def test_over_admission_of_nothing_is_minus_the_limit(self):
        self.assertEqual(bm.over_admission([], 10, 1), -10)

    def test_over_admission_counts_what_passes_the_limit_at_one_instant(self):
        self.assertEqual(bm.over_admission([0.0] * 10, 10, 1), 0)
        self.assertEqual(bm.over_admission([0.0] * 11, 10, 1), 1)

    def test_admissions_at_the_refill_rate_after_a_full_bucket_are_not_over(self):
        times = [0.0] * 10 + [k / 100 for k in range(1, 200)]  # limit 10, then one per 10 ms at 100/s
        self.assertLessEqual(bm.over_admission(times, 10, 100), 1e-9)

    def test_no_demand_admits_nothing(self):
        self.assertEqual(bm.simulate([], 4, 100, 10, 10), [])

    def test_no_check_is_admitted_before_it_arrives(self):  # a check that waits for a refill passes when it lands
        demand = sorted((k * 0.0007, k % 3) for k in range(3000))
        admitted = bm.simulate(demand, 3, 200, 50, 20)
        for i in range(3):
            arrived = sorted(t for t, j in demand if j == i)
            passed = sorted(t for t, j in admitted if j == i)
            self.assertLessEqual(len(passed), len(arrived), i)
            self.assertTrue(all(p >= a for p, a in zip(passed, arrived)), i)  # the k-th admission after the k-th arrival

    def test_the_simulation_is_deterministic(self):
        demand = sorted((k * 0.0013, k % 2) for k in range(2000))
        self.assertEqual(bm.simulate(demand, 2, 100, 30, 10), bm.simulate(demand, 2, 100, 30, 10))

    def test_one_instance_with_a_chunk_of_one_stays_within_one(self):  # N x C = 1
        demand = [(k * 0.0005, 0) for k in range(4000)]
        over = bm.over_admission([t for t, _ in bm.simulate(demand, 1, 50, 100, 1)], 50, 100)
        self.assertLessEqual(over, 1 + 100 * 0.001)

    def test_the_next_chunk_stays_between_the_minimum_and_the_cap(self):  # src/lease/chunk_policy.h
        for cap in (1, 5, 8, 9, 100, 2 ** 31):
            for chunk in (1, 7, 8, cap):
                for emptied in (False, True):
                    for idle in (False, True):
                        c = bm.next_chunk(chunk, emptied, idle, cap)
                        self.assertGreaterEqual(c, min(8, cap))
                        self.assertLessEqual(c, cap)


# ---- Week 7: benchmark tables and the profile summary ----

class Week7MatrixTables(unittest.TestCase):
    @staticmethod
    def run_line(checks, instances=1, engine="leased", cap=10, dist="uniform", allowed=None):
        return json.dumps({"engine": engine, "chunk_pct": cap, "instances": instances, "dist": dist,
                           "checks_per_s": checks, "allowed_per_s": checks if allowed is None else allowed,
                           "p99_ms": 1.0, "errors": 0})

    def table(self, lines):
        return matrix_tables.tables(matrix_tables.load(lines))

    def test_each_instance_count_has_its_own_column(self):
        text = self.table([self.run_line(100, 1), self.run_line(200, 2), self.run_line(400, 4)])
        self.assertIn("| leased, cap 10% | 100 (100 to 100) | 200 (200 to 200) | 400 (400 to 400) |", text)

    def test_large_numbers_get_thousands_separators(self):
        self.assertIn("1,234,567 (1,234,567 to 1,234,567)", self.table([self.run_line(1_234_567)]))

    def test_chunk_caps_are_kept_apart(self):
        text = self.table([self.run_line(100, cap=1)])
        self.assertIn("| leased, cap 1% | 100 (100 to 100) |", text)
        self.assertIn("| leased, cap 10% | - | - | - |", text)

    def test_a_run_with_no_checks_does_not_divide_by_zero(self):
        self.assertIn("| 0% |", self.table([self.run_line(0, allowed=0)]))

    def test_gRPC_alone_is_left_out_of_the_distribution_table(self):
        text = self.table([self.run_line(500, engine="allow_all", cap=None)])
        second = text.split("by key distribution")[1]
        self.assertNotIn("gRPC alone", second)


class Week7ProfileSummary(unittest.TestCase):
    def test_a_frame_name_with_spaces_keeps_its_count(self):
        total, self_by, _ = profile_summary.summarise(["main;operator new(unsigned long) 7"])
        self.assertEqual((total, self_by), (7, {"allocation": 7}))

    def test_lines_without_a_count_are_skipped(self):
        total, _, _ = profile_summary.summarise(["", "main;foo", "main;ratelimiter::Check 3"])
        self.assertEqual(total, 3)

    def test_the_first_matching_category_wins_on_the_leaf(self):
        _, self_by, _ = profile_summary.summarise(["grpc::Run;absl::Mutex::Lock 4", "x;google::protobuf::Arena 2",
                                                   "x;some::Unknown 1"])
        self.assertEqual(self_by, {"locks and atomics": 4, "protobuf": 2, "other": 1})

    def test_a_sample_counts_once_inside_a_category_however_deep(self):
        _, _, inside = profile_summary.summarise(["ratelimiter::A;ratelimiter::B;malloc 10"])
        self.assertEqual(inside["rate limiter"], 10)

    def test_an_empty_profile_reports_without_crashing(self):
        self.assertIn("sample weight (perf periods): 0", profile_summary.report(*profile_summary.summarise([])))


# ---- Week 8: the scalability model and the 1M/s extrapolation ----

@unittest.skipIf(usl_fit is None, "numpy or scipy missing")
class Week8UslFit(unittest.TestCase):
    NS = (1, 2, 4, 8, 16, 32, 64)

    def test_usl_at_one_is_lambda(self):
        self.assertEqual(usl_fit.usl(1, 1234.0, 0.3, 0.01), 1234.0)

    def test_the_fit_recovers_known_parameters(self):
        lam, sigma, kappa, r2, peak, rel = usl_fit.fit([(n, usl_fit.usl(n, 1000, 0.05, 0.001)) for n in self.NS])
        self.assertAlmostEqual(lam / 1000, 1, places=3)
        self.assertAlmostEqual(sigma, 0.05, places=3)
        self.assertAlmostEqual(kappa, 0.001, places=5)
        self.assertGreater(r2, 0.9999)
        self.assertAlmostEqual(peak, math.sqrt(0.95 / 0.001), delta=0.5)

    def test_perfectly_linear_scaling_has_no_peak_and_reports_without_crashing(self):
        rows = {"leased": [(n, {"checks_per_s": 1000.0 * n, "p50_ms": 0.5, "p99_ms": 2.0}) for n in self.NS]}
        text, fits = usl_fit.report(rows)
        self.assertIn("none (kappa about 0)", text)

    def test_littles_law_flags_a_mean_outside_p50_to_p99(self):
        def rows(p50_factor):
            out = []
            for n in self.NS:
                x = usl_fit.usl(n, 1000, 0.05, 0.001)
                mean = 1000 * n / x
                out.append((n, {"checks_per_s": x, "p50_ms": p50_factor * mean, "p99_ms": 2 * mean}))
            return out
        text, _ = usl_fit.report({"strict": rows(0.5), "leased": rows(2.0)})
        little = text.split("Little's law")[1].splitlines()
        self.assertTrue(all(l.endswith("| yes |") for l in little if l.startswith("| strict")), text)
        self.assertTrue(all(l.endswith("| NO |") for l in little if l.startswith("| leased")), text)

    def test_load_groups_runs_by_engine_and_skips_other_lines(self):
        with tempfile.NamedTemporaryFile("w", suffix=".jsonl", delete=False) as f:
            f.write("sweep starting\n" + json.dumps({"engine": "strict", "n": 4, "result": {"checks_per_s": 1}}) + "\n")
        try:
            self.assertEqual(usl_fit.load(f.name), {"strict": [(4, {"checks_per_s": 1})]})
        finally:
            os.remove(f.name)


class Week8Extrapolation(unittest.TestCase):
    def test_every_input_cites_a_file_that_exists_and_holds_the_value(self):
        for name, (value, _, source) in extrapolate.INPUTS.items():
            path = source.split()[0]
            self.assertTrue(os.path.exists(os.path.join(ROOT, path)), (name, path))
            # the evidence states it exactly, or "about" it rounded to two places (refills_cold: 0.217)
            forms = {f"{value:,}", f"{value:g}", f"about {round(value, 2):g}"}
            self.assertTrue(any(form in read(path) for form in forms), (name, value, path))

    def test_masters_and_instances_are_rounded_up_never_down(self):
        rows_m, rows_i = extrapolate.estimate()
        master = extrapolate.v("master")
        for mix, _, calls, m in rows_m:
            self.assertGreaterEqual(m * master, calls, mix)
            self.assertLess((m - 1) * master, calls, mix)
        for what, per, n in rows_i:
            self.assertGreaterEqual(n * per, extrapolate.TARGET, what)
            self.assertLess((n - 1) * per, extrapolate.TARGET, what)

    def test_more_redis_calls_per_check_never_need_fewer_masters(self):
        rows_m, _ = extrapolate.estimate()
        by_calls = sorted((c, m) for _, c, _, m in rows_m)
        self.assertEqual([m for _, m in by_calls], sorted(m for _, m in by_calls))

    def test_cost_per_billion_checks_is_the_month_over_a_months_checks(self):
        for what, _, _, month, per_billion in extrapolate.costs():
            checks = extrapolate.TARGET * 3600 * extrapolate.HOURS
            self.assertAlmostEqual(per_billion * checks / 1e9, month, places=6, msg=what)

    def test_every_computed_table_is_labelled_an_estimate(self):
        text = extrapolate.markdown()
        self.assertEqual(text.count("(an estimate)") + text.count("; an estimate)"), 3, text)


# ---- Week 9: delivery (the usage and coverage reports, and the repository's own consistency) ----

class Week9UsageReport(unittest.TestCase):
    def test_the_fields_are_the_ones_usage_log_writes(self):
        block = re.search(r"kField\{(.*?)\};", read("src/usage/usage_log.cc"), re.S).group(1)
        self.assertEqual(set(usage_report.FIELDS), set(re.findall(r'"([a-z_]+)"', block)))

    def test_events_are_summed_over_instances_and_minutes(self):
        lines = [json.dumps({"instance": i, "customer": "c", "minute": m, "allowed": 1}) for i in "ab" for m in (1, 2)]
        self.assertEqual(usage_report.totals(lines)["c"]["allowed"], 4)

    def test_missing_fields_count_zero_and_unknown_ones_are_ignored(self):
        t = usage_report.totals(['{"customer":"c","minute":1,"colour":5}', "", "   "])
        self.assertEqual(t["c"], dict.fromkeys(usage_report.FIELDS, 0))

    def test_billed_is_allowed_plus_overage(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "u.jsonl")
            with open(path, "w") as f:
                f.write('{"customer":"cust:1","minute":1,"allowed":3,"overage":2,"rate_limited":7}\n')
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                usage_report.main([path])
        row = out.getvalue().splitlines()[1].split()
        self.assertEqual(row[:4], ["cust:1", "5", "3", "2"])  # customer, billed, allowed, overage


class Week9CoverageReport(unittest.TestCase):
    @staticmethod
    def report(*paths):
        return {"current_working_directory": coverage_report.ROOT,
                "files": [{"file": p, "lines": [{"line_number": 1, "count": 1, "branches": []}]} for p in paths]}

    def test_compact_handles_empty_single_and_pairs(self):
        self.assertEqual(coverage_report.compact([]), "")
        self.assertEqual(coverage_report.compact([5]), "5")
        self.assertEqual(coverage_report.compact([1, 2, 4]), "1-2, 4")

    def test_only_the_services_own_code_is_counted(self):
        files = coverage_report.merge([self.report("src/a.cc", "include/b.h", "tests/c.cc", "src/spike/d.cc",
                                                   "/usr/include/e.h", "build/f.pb.cc")])
        self.assertEqual(sorted(files), ["include/b.h", "src/a.cc"])

    def test_paths_are_normalised_before_they_are_judged(self):
        files = coverage_report.merge([self.report("build/../src/a.cc", "src/../tests/x.cc")])
        self.assertEqual(sorted(files), ["src/a.cc"])

    def test_nothing_to_summarise_does_not_divide_by_zero(self):
        self.assertIn("TOTAL", coverage_report.summarise({}))


class Week9Repository(unittest.TestCase):
    def test_every_shell_script_parses(self):
        scripts = subprocess.run(["git", "ls-files", "*.sh", "tools/rlctl"], cwd=ROOT, capture_output=True,
                                 text=True, check=True).stdout.split()
        self.assertGreater(len(scripts), 20)
        for s in scripts:
            r = subprocess.run(["bash", "-n", os.path.join(ROOT, s)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, (s, r.stderr))

    def test_every_script_reproduce_runs_is_executable_in_git(self):  # NTFS: the bit lives only in git
        named = set(re.findall(r"(?:scripts|bench|tests/eval)/[A-Za-z_0-9]+\.sh", read("scripts/reproduce.sh")))
        self.assertTrue(named)
        modes = subprocess.run(["git", "ls-files", "-s", *sorted(named)], cwd=ROOT, capture_output=True,
                               text=True, check=True).stdout.splitlines()
        self.assertEqual(len(modes), len(named), modes)
        for line in modes:
            self.assertTrue(line.startswith("100755"), line)

    def test_every_evidence_file_the_thesis_cites_exists(self):
        cited = set()
        for name in os.listdir(os.path.join(ROOT, "docs", "thesis")):
            if name.endswith(".md"):
                cited |= set(re.findall(r"\b([a-z0-9_]+_20\d\d-\d\d-\d\d\.md)\b", read(f"docs/thesis/{name}")))
        self.assertTrue(cited)
        existing = {n for _, _, names in os.walk(os.path.join(ROOT, "docs")) for n in names}
        self.assertEqual(sorted(cited - existing), [])

    def test_every_tool_is_listed_in_the_tools_readme(self):
        readme = read("tools/README.md")
        for name in os.listdir(os.path.join(ROOT, "tools")):
            if name.endswith((".py", ".sh")) or name == "rlctl":
                self.assertIn(f"`{name}", readme, name)

    def test_every_tool_self_check_passes(self):  # what scripts/reproduce.sh runs first
        tools = [f"tools/{n}" for n in sorted(os.listdir(os.path.join(ROOT, "tools")))
                 if n.endswith(".py") and "--self-check" in read(f"tools/{n}")] + ["tests/eval/bound_model.py"]
        for t in tools:
            if t.endswith("usl_fit.py") and usl_fit is None:
                continue
            r = subprocess.run([sys.executable, os.path.join(ROOT, t), "--self-check"], cwd=ROOT,
                               capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, (t, r.stdout, r.stderr))


if __name__ == "__main__":
    unittest.main()
