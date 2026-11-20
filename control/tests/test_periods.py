"""Periods: the API names a top-up's period as lua/quota.lua does, or the top-up applies to no period.
Known dates, then the script itself on the test Redis (127.0.0.1:6380, RL_TEST_REDIS)."""
import datetime as dt
import json
import os
import pathlib
import unittest

import redis

from rlcontrol import periods

ROOT = pathlib.Path(__file__).resolve().parents[2]


def ms(y, m, d, hh=0, mm=0):
    return int(dt.datetime(y, m, d, hh, mm, tzinfo=dt.timezone.utc).timestamp() * 1000)


class KnownDates(unittest.TestCase):
    def test_days_and_months(self):
        self.assertEqual(periods.period_name(ms(1970, 1, 1), False), "d0")
        self.assertEqual(periods.period_name(ms(2026, 10, 25, 23, 59), False), "d20751")
        self.assertEqual(periods.period_name(ms(2026, 10, 25), True), f"m{2026 * 12 + 9}")
        self.assertEqual(periods.period_name(ms(2026, 12, 31, 23, 59), True), f"m{2026 * 12 + 11}")
        self.assertEqual(periods.period_name(ms(2027, 1, 1), True), f"m{2027 * 12}")

    def test_billing_months(self):
        self.assertEqual(periods.month_id("2026-11"), 2026 * 12 + 10)
        self.assertEqual(periods.month_label(2026 * 12 + 11), "2026-12")
        self.assertEqual(periods.month_of(ms(2026, 12, 31, 23, 59)), periods.month_id("2026-12"))
        self.assertEqual(periods.month_bounds_ms(periods.month_id("2026-12")), (ms(2026, 12, 1), ms(2027, 1, 1)))
        self.assertEqual(periods.month_bounds_ms(periods.month_id("2028-02")), (ms(2028, 2, 1), ms(2028, 3, 1)))
        for bad in ("2026-13", "2026-1", "26-11", "2026/11", "", "2026-00"):
            with self.assertRaises(ValueError, msg=bad):
                periods.month_id(bad)

    def test_reset(self):
        self.assertEqual(periods.reset_after_ms(ms(2026, 10, 25, 23, 0), False), 3_600_000)
        self.assertEqual(periods.reset_after_ms(ms(2026, 12, 31, 23, 0), True), 3_600_000)  # into January
        self.assertEqual(periods.reset_after_ms(ms(2028, 2, 28), True), 2 * 86_400_000)    # a leap year


class Anchored(unittest.TestCase):  # P5: billing from the subscription date
    def test_a_month_runs_from_its_anchor_day_to_the_same_day_of_the_next(self):
        self.assertEqual(periods.month_of(ms(2026, 11, 16), 17), periods.month_id("2026-10"))  # before the 17th
        self.assertEqual(periods.month_of(ms(2026, 11, 17), 17), periods.month_id("2026-11"))
        self.assertEqual(periods.period_name(ms(2027, 1, 5), True, 17), f"m{2026 * 12 + 11}")  # December's, into January
        self.assertEqual(periods.month_bounds_ms(periods.month_id("2026-12"), 17), (ms(2026, 12, 17), ms(2027, 1, 17)))
        self.assertEqual(periods.reset_after_ms(ms(2026, 11, 16, 23), True, 17), 3_600_000)
        self.assertEqual(periods.month_of(ms(2026, 11, 16), 1), periods.month_of(ms(2026, 11, 16)))  # 1: calendar

    def test_an_anchor_is_a_day_from_1_to_28(self):
        self.assertEqual([periods.anchor_of({"anchor": v}) for v in ("17", "0", "29", "x", "")], [17, 1, 1, 1, 1])
        self.assertEqual(periods.anchor_of({}), 1)
        self.assertEqual(periods.anchor_at(ms(2026, 10, 17, 12)), 17)
        self.assertEqual(periods.anchor_at(ms(2026, 10, 30)), 28)  # every month has a 28th


class AgainstTheScript(unittest.TestCase):
    def setUp(self):
        host, _, port = os.environ.get("RL_TEST_REDIS", "127.0.0.1:6380").partition(":")
        self.r = redis.Redis(host=host, port=int(port or 6380), decode_responses=True)
        try:
            self.r.ping()
        except redis.ConnectionError:
            self.skipTest("test Redis not reachable")
        self.quota = self.r.register_script((ROOT / "lua/quota.lua").read_text())

    def test_the_script_names_and_ends_periods_as_the_api_does(self):
        sub = self.r.pubsub()
        sub.subscribe("rl-alerts")
        sub.get_message(timeout=1)  # the subscribe confirmation
        seconds, _ = self.r.time()
        today = periods.anchor_at(seconds * 1000)
        for monthly, anchor in ((False, 1), (True, 1), (True, today), (True, min(today + 1, 28))):
            key = "{tp|c}:daily_quota:q"
            self.r.delete(key)
            reply = self.quota(keys=[key], args=[1, 1, "hard", 0, "month" if monthly else "day", 0, 0, 0, "", anchor])
            seconds, micros = self.r.time()
            now = seconds * 1000 + micros // 1000
            self.assertAlmostEqual(reply[3], periods.reset_after_ms(now, monthly, anchor), delta=1000)
            alert = None
            while alert is None or alert["event"] != "quota.80":
                m = sub.get_message(timeout=2)
                self.assertIsNotNone(m, "no alert from the script")
                if m["type"] == "message":
                    alert = json.loads(m["data"])
            self.assertEqual(alert["period"], periods.period_name(now, monthly, anchor))
        sub.close()


if __name__ == "__main__":
    unittest.main()
