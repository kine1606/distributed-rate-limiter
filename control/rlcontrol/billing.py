"""Billing (Phase 2, P3; docs/PRODUCT_PLAN.md): one invoice per customer and calendar month, made from
the totals lua/quota.lua keeps for the month in the customer's quota counter (fields bm, bu, bo, and
closed:m<month> once a new month began), never from usage events, which are best effort.

An invoice has the plan's monthly fee, the month's billed overage at the plan's price, and each top-up
bought in the month at the price recorded when it was bought. Amounts are integers in the currency's
minor unit (cents), each line rounded half up once. A customer with nothing to bill gets no invoice.
The invoice id names the customer and the month, so closing a month again writes nothing twice, and a
charge carries the invoice as its idempotency key, so nothing is paid twice.

    python -m rlcontrol.billing close [--month YYYY-MM] [--merchant M] [--no-charge]       (from control/)
         (run it daily: a customer billed from its subscription date (P5) is closed once its own month has
         ended, on its day of the next calendar month; reruns write nothing twice)
    python -m rlcontrol.billing retry                     (daily: charges again the invoices whose retry is due)
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

RETRY_AFTER_DAYS = (1, 3, 7)  # P5: a declined charge is tried again after these; the last failure suspends
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


def purchases(r, merchant, start, end):
    """{customer: [(units, price per 1,000, bought at ms)]} of the top-ups bought in [start, end) ms. Stream
    ids are Redis's clock in ms, the clock the quota script counts months by."""
    out = defaultdict(list)
    for _, e in r.xrange(store.purchases_key(merchant), min=str(start), max=str(end - 1)):
        if e.get("kind") != "fee":
            out[e["customer"]].append((int(e["units"]), int(e.get("price", 0)), int(e["at_ms"])))
    return out


def fee_changes(r, merchant, start, end):
    """{customer: [(at ms, fee before, fee after)]} of the changes of a customer's fee in [start, end) ms
    (P5), oldest first, as the API and tools/rlctl record them."""
    out = defaultdict(list)
    for _, e in r.xrange(store.purchases_key(merchant), min=str(start), max=str(end - 1)):
        if e.get("kind") == "fee":
            out[e["customer"]].append((int(e["at_ms"]), int(e["from"]), int(e["to"])))
    return out


def build(merchant, customer, month, plan, totals, bought, now_ms, bounds, changes=()):
    """The invoice of `month` from the customer's plan hash, its totals, its top-ups and the changes of its
    fee in the month. A fee that changed is prorated (P5): each fee for the part of the month it was in
    force, the last one the plan's at close, rounded half up once. A suspended customer still owes the fee
    of the plan it is suspended from."""
    # shortcut: billed overage is priced by the plan in force at close for the whole month; keep the price
    # with each unit in the counter if mid-month price changes matter. Top-ups keep the price they were
    # bought at.
    in_plan, over = totals
    name = plan.get("plan", "free")
    if name == "suspended":
        name = plan.get("resume_plan", "free")
    lines = []
    fee, (start, end) = int(plan.get("fee", 0)), bounds
    parts = []  # (fee, from ms, to ms)
    if changes:
        t, current = start, changes[0][1]
        for at, _, after in changes:
            parts.append((current, t, at))
            t, current = at, after
        parts.append((fee, t, end))
        amount = (2 * sum(f * (b - a) for f, a, b in parts) + (end - start)) // (2 * (end - start))
    else:
        amount = fee
    if amount > 0:
        line = {"item": "fee", "description": f"plan {name}" + (", prorated" if parts else ""), "units": 1,
                "amount": amount}
        if parts:
            line["prorated"] = [{"fee": f, "from_ms": a, "to_ms": b} for f, a, b in parts if b > a]
        lines.append(line)
    if over > 0:
        price = int(plan.get("overage_price", 0))
        lines.append({"item": "overage", "description": "billed overage", "units": over, "price_per_1000": price,
                      "amount": line_amount(over, price)})
    for units, price, at in bought:
        lines.append({"item": "topup", "description": "prepaid top-up", "units": units, "price_per_1000": price,
                      "amount": line_amount(units, price), "bought_at_ms": at})
    total = sum(line["amount"] for line in lines)
    return {"id": invoice_id(customer, month), "merchant": merchant, "customer": customer,
            "month": periods.month_label(month), "period": {"from_ms": bounds[0], "to_ms": bounds[1]},
            "plan": name, "currency": CURRENCY, "lines": lines, "total": total,
            "usage": {"in_plan": in_plan, "billed_overage": over}, "status": "open" if total > 0 else "paid",
            "created_at_ms": now_ms}


def record_payment(r, m, inv, result, provider_name, now):
    """Applies a charge's result to the invoice (P5) and returns its status: paid; pending until the
    provider's webhook settles it; or, failed, retried RETRY_AFTER_DAYS later, the customer suspended only
    once the last attempt failed."""
    inv["payment"] = {"provider": provider_name, "charge": result["id"], "failure": result["failure"]}
    inv.pop("next_attempt_ms", None)
    if result.get("pending"):
        inv["status"] = "pending"
    elif result["ok"]:
        inv["status"] = "paid"
    elif inv["attempts"] <= len(RETRY_AFTER_DAYS):
        inv["status"] = "retrying"
        inv["next_attempt_ms"] = now + RETRY_AFTER_DAYS[inv["attempts"] - 1] * periods.DAY_MS
    else:
        inv["status"] = "payment_failed"
        r.register_script(store.SUSPEND)(keys=[store.plan_key(m, inv["customer"])])
        store.announce(r, m, inv["customer"])
    r.hset(store.invoices_key(m), inv["id"], json.dumps(inv))
    return inv["status"]


def charge(r, provider, m, inv, payer, now):
    """One attempt to charge the invoice; each attempt is its own idempotency key, so a retry is a new
    charge while a rerun of the same attempt is not."""
    inv["attempts"] = inv.get("attempts", 0) + 1
    key = f"{m}/{inv['id']}" + (f"/{inv['attempts']}" if inv["attempts"] > 1 else "")
    return record_payment(r, m, inv, provider.charge(payer, inv["total"], inv["currency"], idempotency_key=key),
                          provider.name, now)


def retry(r, provider, merchants=None):
    """Charges again every invoice whose retry is due; returns {status: count}."""
    now = store.now_ms(r)
    out = defaultdict(int)
    for m in sorted(r.hkeys(store.MERCHANTS)) if merchants is None else merchants:
        for raw in r.hvals(store.invoices_key(m)):
            inv = json.loads(raw)
            if inv["status"] == "retrying" and inv["next_attempt_ms"] <= now and (
                    payer := r.hget(store.payers_key(m), inv["customer"])):
                out[charge(r, provider, m, inv, payer, now)] += 1
    return dict(out)


def settled(r, idempotency_key, ok, failure, charge_id, provider_name):
    """A pending charge settled, as the provider's webhook says: the invoice's new status, or None for a
    charge this platform does not know or one already settled."""
    m, _, rest = idempotency_key.partition("/")
    raw = r.hget(store.invoices_key(m), rest.split("/")[0]) if m else None
    inv = json.loads(raw) if raw else None
    if inv is None or inv["status"] != "pending":
        return None
    return record_payment(r, m, inv, {"id": charge_id, "ok": ok, "pending": False, "failure": failure},
                          provider_name, store.now_ms(r))


def preview(r, merchant, customer):
    """This month's invoice so far, as it would close now (not stored)."""
    now = store.now_ms(r)
    plan = r.hgetall(store.plan_key(merchant, customer))
    anchor = periods.anchor_of(plan)
    month = periods.month_of(now, anchor)
    bounds = periods.month_bounds_ms(month, anchor)
    inv = build(merchant, customer, month, plan, month_totals(r, store.customer_id(merchant, customer), month),
                purchases(r, merchant, *bounds).get(customer, []), now, bounds,
                fee_changes(r, merchant, *bounds).get(customer, []))
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
    charges every open one whose customer has a payment method; a declined charge is retried (`retry`).
    A customer whose month starts on a later day (P5) is skipped until its month has ended too.
    Safe to run again: an invoice is never rewritten, only its payment status, and an open one is charged
    with the same idempotency key. Returns a summary."""
    now = store.now_ms(r)
    if month >= periods.month_of(now):
        raise ValueError(f"{periods.month_label(month)} has not ended")
    found = customers(r)
    names = sorted(r.hkeys(store.MERCHANTS)) if merchants is None else merchants
    summary = {"month": periods.month_label(month), "invoices": 0, "already": 0, "paid": 0, "payment_failed": 0,
               "open": 0, "retrying": 0, "pending": 0, "billed": 0, "not_ended": 0}
    for m in names:
        # every customer's month starts between day 1 of `month` and day 28 of the next
        window = periods.month_bounds_ms(month)[0], periods.month_bounds_ms(month, 28)[1]
        bought, fees = purchases(r, m, *window), fee_changes(r, m, *window)
        for c in sorted(found.get(m, set()) | set(bought)):
            counter = store.quota_key(m, c)
            plan = r.hgetall(store.plan_key(m, c))
            bounds = periods.month_bounds_ms(month, periods.anchor_of(plan))
            if bounds[1] > now:
                summary["not_ended"] += 1
                continue
            inv = build(m, c, month, plan, month_totals(r, store.customer_id(m, c), month),
                        [b for b in bought.get(c, []) if bounds[0] <= b[2] < bounds[1]], now, bounds,
                        [f for f in fees.get(c, []) if bounds[0] <= f[0] < bounds[1]])
            if inv["lines"]:
                if r.hsetnx(store.invoices_key(m), inv["id"], json.dumps(inv)):
                    summary["invoices"] += 1
                else:
                    inv = json.loads(r.hget(store.invoices_key(m), inv["id"]))
                    summary["already"] += 1
                if provider is not None and inv["status"] == "open" and (payer := r.hget(store.payers_key(m), c)):
                    charge(r, provider, m, inv, payer, now)
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
    # shortcut: events are counted by calendar month; for a customer billed from its subscription date
    # (P5) compare across its own month's bounds (periods.month_bounds_ms with its anchor) if needed.
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
    sub.add_parser("retry", help="charge again the invoices whose retry is due")
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
    elif args.command == "retry":
        print(json.dumps(retry(r, SandboxProvider(r))))
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
