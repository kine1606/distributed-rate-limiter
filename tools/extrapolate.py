#!/usr/bin/env python3
"""Chapter 5.11: what 1,000,000 checks per second would need, from per-instance and per-master
measurements on one laptop (the model of Chapter 3.8, with the final inputs). Every input names its
evidence file; every output is an ESTIMATE, never a measurement.

    tools/extrapolate.py              prints the inputs and the estimate as Markdown
    tools/extrapolate.py --self-check
"""
import math
import sys

TARGET = 1_000_000
E = "docs/thesis/evidence/"
INPUTS = {  # name: (value, what, source)
    "lease_closed": (67_459, "leased rule (default cap 10%), one instance, 192 in flight, Zipf over 1M keys",
                     E + "matrix_2026-10-24.md"),
    "lease_hot": (95_913, "leased rule, one instance, one hot key", E + "matrix_2026-10-24.md"),
    "lease_g6": (20_000, "leased rule, highest fixed rate tried with p99 < 2 ms (1.45 ms; 30,000/s gave 2.54 ms)",
                 E + "open_loop_2026-10-24.md"),
    "lease_batch": (400_187, "leased rule, one instance, CheckBatch of 10, Zipf over 1M keys", E + "matrix_2026-10-24.md"),
    "customer_closed": (31_519, "customer check, one instance, 192 in flight, Zipf over 1M Pay customers",
                        E + "matrix_2026-10-24.md"),
    "customer_open": (20_000, "customer check, highest fixed rate tried, all answered, p99 7.84 ms",
                      E + "open_loop_2026-10-24.md"),
    "master": (120_000, "script calls one Redis master absorbs", "docs/DESIGN.md section 3 (Week 1 spike)"),
    "refills_hot": (0.017, "refills per leased check, Zipf over 10k keys", E + "scaling_2026-10-24.md"),
    "refills_cold": (0.217, "refills per leased check, Zipf over 1M keys", E + "scaling_2026-10-24.md"),
    "plan_reads_worst": (0.89, "plan reads per customer check, 1M customers, uniform", E + "flood_cardinality_2026-10-22.md"),
}


# On-demand list prices, retrieved 2026-10-24 from instances.vantage.sh (its default, us-east-1 Linux /
# Redis OSS; the page names neither, the figures match AWS's published list prices for these types).
PRICES = {
    "instance": (0.357, "c7i.2xlarge, 8 vCPU, 16 GiB: one limiter instance (the laptop gave each 6 CPUs)",
                 "https://instances.vantage.sh/aws/ec2/c7i.2xlarge"),
    "redis": (0.219, "cache.r7g.large, 2 vCPU, 13.07 GiB: one Redis master or replica",
              "https://instances.vantage.sh/aws/elasticache/cache.r7g.large"),
}
HOURS = 730  # a month


def v(name):
    return INPUTS[name][0]


def estimate():
    masters = [  # mix, Redis calls per check
        ("a leased rule, hot keys (10k active)", v("refills_hot")),
        ("a leased rule, 1M keys (Zipf)", v("refills_cold")),
        ("customer checks, enterprise plans (quota leased)", 1 + v("refills_hot")),
        ("customer checks, Free and Pay, plans cached", 2),
        ("customer checks, Free and Pay, worst case for the plan cache", 2 + v("plan_reads_worst")),
    ]
    rows_m = [(mix, c, TARGET * c, math.ceil(TARGET * c / v("master"))) for mix, c in masters]
    instances = [
        ("leased rule, saturated (p99 about 7 ms)", v("lease_closed")),
        ("leased rule, p99 below 2 ms (goal G6)", v("lease_g6")),
        ("leased rule, CheckBatch of 10", v("lease_batch")),
        ("customer check, saturated (p99 about 13 ms)", v("customer_closed")),
        ("customer check, at a rate answered in full (p99 7.8 ms)", v("customer_open")),
    ]
    rows_i = [(what, per, math.ceil(TARGET / per)) for what, per in instances]
    return rows_m, rows_i


def costs():
    """Monthly on-demand cost of three deployments at 1,000,000 checks/s, each master with one replica."""
    _, rows_i = estimate()
    inst = dict((r[0], r[2]) for r in rows_i)
    rows_m, _ = estimate()
    mast = dict((r[0], r[3]) for r in rows_m)
    plans = [
        ("leased rule, p99 below 2 ms", inst["leased rule, p99 below 2 ms (goal G6)"], mast["a leased rule, 1M keys (Zipf)"]),
        ("leased rule, CheckBatch of 10", inst["leased rule, CheckBatch of 10"], mast["a leased rule, 1M keys (Zipf)"]),
        ("customer checks, Free and Pay, answered in full", inst["customer check, at a rate answered in full (p99 7.8 ms)"],
         mast["customer checks, Free and Pay, plans cached"]),
    ]
    out = []
    for what, n, m in plans:
        month = (n * PRICES["instance"][0] + 2 * m * PRICES["redis"][0]) * HOURS
        out.append((what, n, m, month, month / (TARGET * 3600 * HOURS / 1e9)))
    return out


def markdown():
    rows_m, rows_i = estimate()
    out = ["Inputs:", "", "| Input | Value | Source |", "| --- | --- | --- |"]
    out += [f"| {what} | {val:,} | `{src}` |" for val, what, src in INPUTS.values()]
    out += ["", "Redis masters at 1,000,000 checks/s (an estimate):", "",
            "| All checks are | Redis calls per check | Calls/s | Masters at 120,000 |", "| --- | --- | --- | --- |"]
    out += [f"| {mix} | {c:g} | {calls:,.0f} | {m} |" for mix, c, calls, m in rows_m]
    out += ["", "Limiter instances at 1,000,000 checks/s (an estimate):", "",
            "| Operating point | Checks/s per instance | Instances |", "| --- | --- | --- |"]
    out += [f"| {what} | {per:,} | {n} |" for what, per, n in rows_i]
    out += ["", "Prices (on demand, per hour, retrieved 2026-10-24):", "", "| Item | Price | Source |", "| --- | --- | --- |"]
    out += [f"| {what} | ${price} | {src} |" for price, what, src in PRICES.values()]
    out += ["", f"Monthly cost at 1,000,000 checks/s ({HOURS} hours, one replica per master; an estimate):", "",
            "| Deployment | Instances | Masters (+ replicas) | Per month | Per billion checks |", "| --- | --- | --- | --- | --- |"]
    out += [f"| {what} | {n} | {m} (+{m}) | ${month:,.0f} | ${per_b:.2f} |" for what, n, m, month, per_b in costs()]
    return "\n".join(out) + "\n"


def self_check():
    rows_m, rows_i = estimate()
    assert dict((m[0], m[3]) for m in rows_m)["customer checks, Free and Pay, plans cached"] == 17
    assert dict((r[0], r[2]) for r in rows_i)["leased rule, p99 below 2 ms (goal G6)"] == 50
    assert "| 1,000,000 |" not in markdown()  # no output dressed as a measurement
    c = dict((r[0], r[3]) for r in costs())
    assert abs(c["leased rule, p99 below 2 ms"] - (50 * 0.357 + 2 * 2 * 0.219) * 730) < 0.01
    print("extrapolate self-check: ok")


if __name__ == "__main__":
    self_check() if sys.argv[1:] == ["--self-check"] else sys.stdout.write(markdown())
