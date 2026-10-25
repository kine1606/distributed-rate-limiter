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

    def test_reset(self):
        self.assertEqual(periods.reset_after_ms(ms(2026, 10, 25, 23, 0), False), 3_600_000)
        self.assertEqual(periods.reset_after_ms(ms(2026, 12, 31, 23, 0), True), 3_600_000)  # into January
        self.assertEqual(periods.reset_after_ms(ms(2028, 2, 28), True), 2 * 86_400_000)    # a leap year


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
        for monthly in (False, True):
            key = "{tp|c}:daily_quota:q"
            self.r.delete(key)
            reply = self.quota(keys=[key], args=[1, 1, "hard", 0, "month" if monthly else "day"])
            seconds, micros = self.r.time()
            now = seconds * 1000 + micros // 1000
            self.assertAlmostEqual(reply[3], periods.reset_after_ms(now, monthly), delta=1000)
            alert = None
            while alert is None or alert["event"] != "quota.80":
                m = sub.get_message(timeout=2)
                self.assertIsNotNone(m, "no alert from the script")
                if m["type"] == "message":
                    alert = json.loads(m["data"])
            self.assertEqual(alert["period"], periods.period_name(now, monthly))
        sub.close()


if __name__ == "__main__":
    unittest.main()
