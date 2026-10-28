"""Billing (Phase 2, P3; docs/PRODUCT_PLAN.md): one invoice per customer and calendar month, made from
the totals lua/quota.lua keeps for the month in the customer's quota counter (fields bm, bu, bo, and
closed:m<month> once a new month began), never from usage events, which are best effort.

An invoice has the plan's monthly fee, the month's billed overage at the plan's price, and each top-up
bought in the month at the price recorded when it was bought. Amounts are integers in the currency's
minor unit (cents), each line rounded half up once. A customer with nothing to bill gets no invoice.
The invoice id names the customer and the month, so closing a month again writes nothing twice, and a
charge carries the invoice as its idempotency key, so nothing is paid twice.

    python -m rlcontrol.billing close [--month YYYY-MM] [--merchant M] [--no-charge]       (from control/)
    python -m rlcontrol.billing export --merchant M --month YYYY-MM [--csv]
    python -m rlcontrol.billing reconcile --month YYYY-MM <usage file> ...  (each limiter's RL_USAGE_FILE)
They read RL_REDIS (default 127.0.0.1:7001). `close` defaults to the month that ended last.
"""
import argparse
import csv
import io
import json
import os
import sys
from collections import defaultdict

from . import periods, store

CURRENCY = "USD"  # shortcut: one currency for every merchant; make it a merchant setting when one bills in another
CSV_FIELDS = ("id", "merchant", "customer", "month", "plan", "currency", "fee", "overage_units", "overage",
              "topup_units", "topups", "total", "status", "charge")


def line_amount(units, price_per_1000):
    return (units * price_per_1000 + 500) // 1000


def invoice_id(customer, month):
    return f"in_{periods.month_label(month)}_{customer}"  # unique: customer ids have no "_" (api.CUSTOMER)


def month_totals(r, limiter_id, month):
    """(units within the plan, billed overage units) of `month` for the customer the limiter calls
    `limiter_id`, as the quota script kept them. The first is None once a month without billed overage
    has ended: the script files only months with something to bill."""
    bm, bu, bo, closed = r.hmget(store.counter_key(limiter_id), "bm", "bu", "bo", f"closed:m{month}")
    if bm is not None and int(bm) == month:
        return int(bu or 0), int(bo or 0)
    if closed:
        bu, bo = closed.split(",")
        return int(bu), int(bo)
    return None, 0


def purchases(r, merchant, month):
    """{customer: [(units, price per 1,000, bought at ms)]} of the top-ups bought in `month`. Stream ids
    are Redis's clock in ms, the clock the quota script counts months by."""
    start, end = periods.month_bounds_ms(month)
    out = defaultdict(list)
    for _, e in r.xrange(store.purchases_key(merchant), min=str(start), max=str(end - 1)):
        out[e["customer"]].append((int(e["units"]), int(e.get("price", 0)), int(e["at_ms"])))
    return out


def build(merchant, customer, month, plan, totals, bought, now_ms):
    """The invoice of `month` from the customer's plan hash, its totals and its top-ups. A suspended
    customer still owes the fee of the plan it is suspended from."""
    # shortcut: the fee and the overage price are those of the plan in force at close, for the whole
    # month (no proration); keep the price with each unit in the counter if mid-month changes matter
    # (P5). Top-ups already keep the price they were bought at.
    in_plan, over = totals
    name = plan.get("plan", "free")
    if name == "suspended":
        name = plan.get("resume_plan", "free")
    lines = []
    if (fee := int(plan.get("fee", 0))) > 0:
        lines.append({"item": "fee", "description": f"plan {name}", "units": 1, "amount": fee})
    if over > 0:
        price = int(plan.get("overage_price", 0))
        lines.append({"item": "overage", "description": "billed overage", "units": over, "price_per_1000": price,
                      "amount": line_amount(over, price)})
    for units, price, at in bought:
        lines.append({"item": "topup", "description": "prepaid top-up", "units": units, "price_per_1000": price,
                      "amount": line_amount(units, price), "bought_at_ms": at})
    total = sum(line["amount"] for line in lines)
    return {"id": invoice_id(customer, month), "merchant": merchant, "customer": customer,
            "month": periods.month_label(month), "plan": name, "currency": CURRENCY, "lines": lines, "total": total,
            "usage": {"in_plan": in_plan, "billed_overage": over}, "status": "open" if total > 0 else "paid",
            "created_at_ms": now_ms}


def preview(r, merchant, customer):
    """This month's invoice so far, as it would close now (not stored)."""
    now = store.now_ms(r)
    month = periods.month_of(now)
    inv = build(merchant, customer, month, r.hgetall(store.plan_key(merchant, customer)),
                month_totals(r, store.customer_id(merchant, customer), month),
                purchases(r, merchant, month).get(customer, []), now)
    inv["status"] = "preview"
    return inv


def customers(r):
    """{merchant: {customer}} of every merchant customer with a plan hash ({M|C}:plan), on every master."""
    out = defaultdict(set)
    for key in r.scan_iter(match="{*|*}:plan", count=1000):
        merchant, _, customer = key[1:-len("}:plan")].partition("|")
        out[merchant].add(customer)
    return out


def close(r, month, provider=None, merchants=None):
    """Writes the invoices of `month`, which must have ended by Redis's clock, and with a `provider`
    charges every open one whose customer has a payment method; a declined charge suspends the customer.
    Safe to run again: an invoice is never rewritten, only its payment status, and an open one is charged
    with the same idempotency key. Returns a summary."""
    now = store.now_ms(r)
    if month >= periods.month_of(now):
        raise ValueError(f"{periods.month_label(month)} has not ended")
    suspend = r.register_script(store.SUSPEND)
    found = customers(r)
    names = sorted(r.hkeys(store.MERCHANTS)) if merchants is None else merchants
    summary = {"month": periods.month_label(month), "invoices": 0, "already": 0, "paid": 0, "payment_failed": 0,
               "open": 0, "billed": 0}
    for m in names:
        bought = purchases(r, m, month)
        for c in sorted(found.get(m, set()) | set(bought)):
            counter = store.quota_key(m, c)
            inv = build(m, c, month, r.hgetall(store.plan_key(m, c)), month_totals(r, store.customer_id(m, c), month),
                        bought.get(c, []), now)
            if inv["lines"]:
                if r.hsetnx(store.invoices_key(m), inv["id"], json.dumps(inv)):
                    summary["invoices"] += 1
                else:
                    inv = json.loads(r.hget(store.invoices_key(m), inv["id"]))
                    summary["already"] += 1
                if provider is not None and inv["status"] == "open" and (payer := r.hget(store.payers_key(m), c)):
                    result = provider.charge(payer, inv["total"], inv["currency"], idempotency_key=f"{m}/{inv['id']}")
                    inv["status"] = "paid" if result["ok"] else "payment_failed"
                    inv["payment"] = {"provider": provider.name, "charge": result["id"], "failure": result["failure"]}
                    r.hset(store.invoices_key(m), inv["id"], json.dumps(inv))
                    if not result["ok"]:
                        suspend(keys=[store.plan_key(m, c)])
                        store.announce(r, m, c)
                summary[inv["status"]] += 1
                summary["billed"] += inv["total"]
            r.hdel(counter, f"closed:m{month}")  # invoiced: the script's copy can go
    return summary


def invoices(r, merchant, month):
    label = periods.month_label(month)
    found = (json.loads(v) for v in r.hgetall(store.invoices_key(merchant)).values())
    return sorted((i for i in found if i["month"] == label), key=lambda i: i["id"])


def _cell(text):  # a spreadsheet would run a cell starting with = + - @ as a formula
    return "'" + text if text[:1] in ("=", "+", "-", "@", "\t", "\r") else text


def to_csv(rows):
    out = io.StringIO()
    w = csv.writer(out, lineterminator="\n")
    w.writerow(CSV_FIELDS)
    for i in rows:
        def total(item, key):
            return sum(line[key] for line in i["lines"] if line["item"] == item)
        w.writerow([_cell(i["id"]), _cell(i["merchant"]), _cell(i["customer"]), i["month"], _cell(i["plan"]),
                    i["currency"], total("fee", "amount"), total("overage", "units"), total("overage", "amount"),
                    total("topup", "units"), total("topup", "amount"), i["total"], i["status"],
                    i.get("payment", {}).get("charge", "")])
    return out.getvalue()


def reconcile(r, month, lines):
    """Per customer, what the limiters' usage events recorded in `month` (allowed, overage) against the
    quota counter's totals (in the plan, billed overage); customers with billed overage but no events
    are included. Events carry their instance's UTC minute, so a call in the last seconds of a month
    can count in the next one if that clock is off; a leased quota counts grants, not admissions, in the
    plan. Every difference is reported, none is corrected."""
    start, end = periods.month_bounds_ms(month)
    seen = defaultdict(lambda: [0, 0])
    for line in lines:
        if line.strip():
            e = json.loads(line)
            if start <= e["minute"] * 60_000 < end:
                seen[e["customer"]][0] += e.get("allowed", 0)
                seen[e["customer"]][1] += e.get("overage", 0)
    suffix = f"}}:{store.QUOTA_RULE}:q"
    for key in r.scan_iter(match="{*" + suffix, count=1000):
        cid = key[1:-len(suffix)]
        if cid not in seen and month_totals(r, cid, month)[1] > 0:
            seen[cid] = [0, 0]
    rows = []
    for cid, (allowed, over) in sorted(seen.items()):
        in_plan, billed = month_totals(r, cid, month)
        rows.append({"customer": cid, "events_allowed": allowed, "counter_in_plan": in_plan,
                     "events_overage": over, "counter_overage": billed, "overage_difference": billed - over})
    return rows


def main(argv=None):
    p = argparse.ArgumentParser(prog="python -m rlcontrol.billing", description=__doc__.split("\n\n")[0])
    sub = p.add_subparsers(dest="command", required=True)
    c = sub.add_parser("close", help="invoice (and charge) an ended month")
    c.add_argument("--month", help="YYYY-MM; default: the month that ended last")
    c.add_argument("--merchant", action="append", help="only this merchant (repeatable)")
    c.add_argument("--no-charge", action="store_true", help="write invoices, charge nobody")
    e = sub.add_parser("export", help="a merchant's invoices of a month, as JSON or CSV")
    e.add_argument("--merchant", required=True)
    e.add_argument("--month", required=True)
    e.add_argument("--csv", action="store_true")
    rc = sub.add_parser("reconcile", help="usage events against the quota counters")
    rc.add_argument("--month", required=True)
    rc.add_argument("files", nargs="+")
    args = p.parse_args(argv)

    from redis.cluster import RedisCluster  # here, so the module imports without a cluster client
    from .payments import SandboxProvider
    host, port = os.environ.get("RL_REDIS", "127.0.0.1:7001").rsplit(":", 1)
    r = RedisCluster(host=host, port=int(port), decode_responses=True)
    if args.command == "close":
        month = periods.month_id(args.month) if args.month else periods.month_of(store.now_ms(r)) - 1
        print(json.dumps(close(r, month, None if args.no_charge else SandboxProvider(r), args.merchant)))
    elif args.command == "export":
        rows = invoices(r, args.merchant, periods.month_id(args.month))
        sys.stdout.write(to_csv(rows) if args.csv else json.dumps(rows, indent=2) + "\n")
    else:
        lines = []
        for path in args.files:
            with open(path, encoding="ascii") as f:  # UsageLog escapes everything else
                lines += f.readlines()
        rows = reconcile(r, periods.month_id(args.month), lines)
        print("customer", "events_allowed", "counter_in_plan", "events_overage", "counter_overage", "difference", sep="\t")
        for row in rows:
            print(*row.values(), sep="\t")
        off = [row for row in rows if row["overage_difference"]]
        print(f"{len(rows)} customers; billed overage differs for {len(off)}", file=sys.stderr)
        return 1 if off else 0
    return 0


if __name__ == "__main__":
    sys.exit(main())
