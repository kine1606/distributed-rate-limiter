#!/usr/bin/env python3
"""Sums usage-event files (src/usage/README.md) per customer, across instances and minutes.

    tools/usage_report.py <file> [<file> ...]    one file per limiter instance (RL_USAGE_FILE)
    tools/usage_report.py --self-check

Prints one row per customer; the numbers are cost units (requests when cost is 1). billed is
allowed + overage: unverified admissions (Redis was down) are not billed (docs/CUSTOMER_PLANS.md).
"""
import json
import sys
from collections import defaultdict

FIELDS = ("allowed", "overage", "unverified", "rate_limited", "quota_exhausted", "unavailable", "suspended")


def totals(lines):
    """{customer: {field: sum}} over JSON lines, as written by UsageLog."""
    out = defaultdict(lambda: dict.fromkeys(FIELDS, 0))
    for line in lines:
        if line.strip():
            event = json.loads(line)
            for f in FIELDS:
                out[event["customer"]][f] += event.get(f, 0)
    return out


def main(paths):
    lines = []
    for path in paths:
        with open(path, encoding="ascii") as f:  # UsageLog escapes everything else
            lines += f.readlines()
    rows = totals(lines)
    width = max([len("customer")] + [len(c) for c in rows])
    print("customer".ljust(width), *(f"{f:>15}" for f in ("billed",) + FIELDS))
    for customer in sorted(rows):
        t = rows[customer]
        print(customer.ljust(width), *(f"{n:>15}" for n in [t["allowed"] + t["overage"]] + [t[f] for f in FIELDS]))


def self_check():
    a = '{"instance":"a","customer":"cust:1","minute":1,"allowed":3,"overage":1}\n'
    b = '{"instance":"b","customer":"cust:1","minute":2,"allowed":2,"rate_limited":5}\n\n'
    c = '{"instance":"b","customer":"cust:\\"2","minute":2,"suspended":1}\n'
    t = totals([a, b, c])
    assert t["cust:1"]["allowed"] == 5 and t["cust:1"]["overage"] == 1 and t["cust:1"]["rate_limited"] == 5, t
    assert t['cust:"2']["suspended"] == 1 and t['cust:"2']["allowed"] == 0, t
    print("usage_report self-check passed")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    elif len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        main(sys.argv[1:])
    else:
        sys.exit(__doc__)
