"""Billing and payments (P3) against the dev cluster (127.0.0.1:7001, RL_TEST_CLUSTER): invoices of an
ended month from what lua/quota.lua kept, once however often the month is closed; charges through the
payment sandbox, a declined one suspending the customer; never a card number; the preview of this month
with the real script; CSV export; reconciliation against usage events. Every merchant is new and every
key it wrote is deleted afterwards."""
import csv
import io
import json
import pathlib
import secrets
import unittest

from fastapi.testclient import TestClient
from redis.exceptions import ConnectionError as RedisConnectionError
from redis.exceptions import RedisClusterException

from rlcontrol import billing, payments, periods, store
from rlcontrol.api import create_app

from .test_api import ADMIN, PLAN, cluster

ROOT = pathlib.Path(__file__).resolve().parents[2]
PAY = {**PLAN, "overage": "postpaid", "allowance": 100_000, "fee": 1900, "overage_price": 50, "topup_price": 40}


class BillingTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.r = cluster()
            cls.r.ping()
        except (RedisClusterException, RedisConnectionError):
            raise unittest.SkipTest("Redis Cluster not reachable")
        cls.provider = payments.SandboxProvider(cls.r)
        cls.client = TestClient(create_app(redis=cls.r, admin_token=ADMIN, provider=cls.provider))
        cls.quota = cls.r.register_script((ROOT / "lua/quota.lua").read_text())

    def setUp(self):
        self.merchants, self.customers = [], ["u1", "u2", "u3", "-x1"]
        self.month = periods.month_of(store.now_ms(self.r))
        self.last = self.month - 1

    def tearDown(self):
        for m in self.merchants:
            for pid in self.r.hvals(store.payers_key(m)):
                self.r.hdel(payments.CUSTOMERS, pid)
            for iid in self.r.hkeys(store.invoices_key(m)):
                self.r.hdel(payments.CHARGES, f"{m}/{iid}")
            for key in (store.catalog_key(m), store.invoices_key(m), store.purchases_key(m), store.payers_key(m)):
                self.r.delete(key)
            for c in self.customers:
                self.r.delete(store.plan_key(m, c), store.quota_key(m, c))
            for h in (store.KEYS_HASH, store.ADMIN_HASH):
                mine = [k for k, owner in self.r.hgetall(h).items() if owner == m]
                if mine:
                    self.r.hdel(h, *mine)
            self.r.hdel(store.MERCHANTS, m)

    def merchant(self, plans=(("pay", PAY), ("free", PLAN))):
        m = "b" + secrets.token_hex(5)
        res = self.client.post("/v1/merchants", json={"id": m}, headers={"Authorization": f"Bearer {ADMIN}"})
        self.merchants.append(m)
        auth = {"Authorization": f"Bearer {res.json()['management_key']}"}
        for name, terms in plans:
            self.assertEqual(self.client.put(f"/v1/plans/{name}", json=terms, headers=auth).status_code, 200)
        return m, auth

    def customer(self, auth, cid, plan="pay"):
        self.assertEqual(self.client.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth).status_code, 201)

    def lines(self, invoice):
        return {line["item"]: (line["units"], line["amount"]) for line in invoice["lines"]}

    def test_an_ended_month_is_invoiced_from_the_counters_once(self):
        m, auth = self.merchant()
        for cid, plan in (("u1", "pay"), ("u2", "pay"), ("u3", "free")):
            self.customer(auth, cid, plan)
        # Last month as the quota script left it: u1's counter still holds it (no call since), u2's
        # filed it when this month began; u1 also bought a top-up then.
        self.r.hset(store.quota_key(m, "u1"), mapping={"bm": self.last, "bu": 10, "bo": 2345})
        self.r.hset(store.quota_key(m, "u2"), mapping={"bm": self.month, "bu": 3, "bo": 0, f"closed:m{self.last}": "8,1000"})
        start = periods.month_bounds_ms(self.last)[0]
        self.r.xadd(store.purchases_key(m), {"customer": "u1", "units": 1000, "price": 40, "at_ms": start + 5,
                                             "period": f"m{self.last}"}, id=f"{start + 5}-0")

        summary = billing.close(self.r, self.last, merchants=[m])
        self.assertEqual((summary["invoices"], summary["already"], summary["open"]), (2, 0, 2))  # Free owes nothing
        u1, u2 = billing.invoices(self.r, m, self.last)
        self.assertEqual(self.lines(u1), {"fee": (1, 1900), "overage": (2345, 117), "topup": (1000, 40)})  # 117.25
        self.assertEqual((u1["total"], u1["usage"]), (2057, {"in_plan": 10, "billed_overage": 2345}))
        self.assertEqual(self.lines(u2), {"fee": (1, 1900), "overage": (1000, 50)})
        self.assertEqual(u2["id"], f"in_{periods.month_label(self.last)}_u2")
        self.assertIsNone(self.r.hget(store.quota_key(m, "u2"), f"closed:m{self.last}"))  # invoiced: dropped
        self.assertEqual(self.r.hget(store.quota_key(m, "u2"), "bu"), "3")  # this month untouched

        again = billing.close(self.r, self.last, merchants=[m])
        self.assertEqual((again["invoices"], again["already"], again["billed"]), (0, 2, summary["billed"]))
        self.assertEqual(billing.invoices(self.r, m, self.last), [u1, u2])
        with self.assertRaises(ValueError):
            billing.close(self.r, self.month, merchants=[m])  # not over yet

    def test_charges_once_and_a_declined_card_suspends(self):
        m, auth = self.merchant()
        for cid in ("u1", "u2", "u3"):
            self.customer(auth, cid)
        for card in ("4242 4242 4242 4242", "4242424242424242", "tok_4242424242424242"):
            res = self.client.put("/v1/customers/u1/payment-method", json={"token": card}, headers=auth)
            self.assertEqual(res.status_code, 422, card)
            self.assertEqual(res.json()["detail"]["code"], "card_number_refused")
        self.assertEqual(self.client.put("/v1/customers/u1/payment-method", json={"token": "tok_nope"},
                                         headers=auth).status_code, 422)
        self.assertEqual(self.client.put("/v1/customers/ghost/payment-method", json={"token": "tok_visa"},
                                         headers=auth).status_code, 404)
        res = self.client.put("/v1/customers/u1/payment-method", json={"token": "tok_visa"}, headers=auth)
        self.assertEqual(res.json()["card"], {"brand": "visa", "last4": "4242"})
        self.client.put("/v1/customers/u2/payment-method", json={"token": "tok_chargeDeclined"}, headers=auth)
        self.assertNotIn("4242", json.dumps(self.r.hgetall(store.payers_key(m))))  # provider ids only

        summary = billing.close(self.r, self.last, self.provider, [m])  # each owes last month's fee
        self.assertEqual((summary["paid"], summary["payment_failed"], summary["open"]), (1, 1, 1))
        u1, u2, u3 = billing.invoices(self.r, m, self.last)
        self.assertEqual((u1["status"], u1["payment"]["failure"]), ("paid", None))
        self.assertEqual((u2["status"], u2["payment"]["failure"]), ("payment_failed", "card_declined"))
        self.assertEqual(u3["status"], "open")  # no card: for the merchant to collect
        self.assertEqual(self.r.hget(store.plan_key(m, "u2"), "plan"), "suspended")
        self.assertEqual(self.client.get("/v1/customers/u2", headers=auth).json()["suspended"], True)
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "plan"), "pay")

        # u3 adds a card; closing again charges u3 only, and u1's charge is not repeated.
        self.client.put("/v1/customers/u3/payment-method", json={"token": "tok_mastercard"}, headers=auth)
        again = billing.close(self.r, self.last, self.provider, [m])
        self.assertEqual((again["invoices"], again["paid"], again["payment_failed"]), (0, 2, 1))
        charged = [k for k in self.r.hkeys(payments.CHARGES) if k.startswith(m + "/")]
        self.assertEqual(len(charged), 3)
        self.assertEqual(billing.invoices(self.r, m, self.last)[0]["payment"], u1["payment"])

    def test_this_months_preview_counts_what_the_script_admits(self):
        m, auth = self.merchant()
        self.customer(auth, "u1")
        res = self.client.post("/v1/customers/u1/topups", json={"units": 500}, headers=auth)
        self.assertEqual((res.json()["price_per_1000"], res.json()["amount"]), (40, 20))
        counter = store.quota_key(m, "u1")
        for cost in (10, 7):  # as the limiter calls it on a limit of 10: the plan's 10, then 7 billed
            self.quota(keys=[counter], args=[10, cost, "cap", 100_000, "day", 0, 0, 0, ""])
        preview = self.client.get("/v1/customers/u1/invoice-preview", headers=auth).json()
        self.assertEqual(preview["status"], "preview")
        self.assertEqual(self.lines(preview), {"fee": (1, 1900), "overage": (7, 0), "topup": (500, 20)})
        self.assertEqual(preview["usage"], {"in_plan": 10, "billed_overage": 7})
        self.assertEqual(self.client.get("/v1/customers/ghost/invoice-preview", headers=auth).status_code, 404)

    def test_csv_export_and_the_operator_close(self):
        m, auth = self.merchant()
        for cid in ("u1", "-x1"):
            self.customer(auth, cid)
        op = {"Authorization": f"Bearer {ADMIN}"}
        label = periods.month_label(self.last)
        self.assertEqual(self.client.post(f"/v1/billing/close?month={periods.month_label(self.month)}&merchant={m}",
                                          headers=op).status_code, 409)
        self.assertEqual(self.client.post(f"/v1/billing/close?month={label}&merchant={m}", headers=auth).status_code, 401)
        res = self.client.post(f"/v1/billing/close?month={label}&merchant={m}&charge=false", headers=op)
        self.assertEqual(res.json()["invoices"], 2)
        res = self.client.get(f"/v1/invoices.csv?month={label}", headers=auth)
        self.assertEqual(res.headers["content-type"].split(";")[0], "text/csv")
        rows = list(csv.DictReader(io.StringIO(res.text)))
        self.assertEqual([row["customer"] for row in rows], ["'-x1", "u1"])  # never read as a formula
        self.assertEqual((rows[1]["fee"], rows[1]["total"], rows[1]["status"]), ("1900", "1900", "open"))
        self.assertEqual(len(self.client.get(f"/v1/invoices?month={label}", headers=auth).json()), 2)
        self.assertEqual(self.client.get("/v1/invoices?month=2026-13", headers=auth).status_code, 422)

    def test_reconciliation_reports_every_difference(self):
        m, auth = self.merchant()
        for cid in ("u1", "u2"):
            self.customer(auth, cid)
        u1, u2 = store.customer_id(m, "u1"), store.customer_id(m, "u2")
        self.r.hset(store.quota_key(m, "u1"), mapping={"bm": self.last, "bu": 10, "bo": 5})
        self.r.hset(store.quota_key(m, "u2"), mapping={"bm": self.last, "bu": 2, "bo": 3})  # no events at all
        start, end = periods.month_bounds_ms(self.last)
        events = [json.dumps({"instance": "a", "customer": u1, "minute": start // 60_000, "allowed": 6, "overage": 4}),
                  json.dumps({"instance": "b", "customer": u1, "minute": end // 60_000 - 1, "allowed": 4}),
                  json.dumps({"instance": "b", "customer": u1, "minute": end // 60_000, "overage": 9})]  # next month
        rows = {row["customer"]: row for row in billing.reconcile(self.r, self.last, events)}
        self.assertEqual(rows[u1], {"customer": u1, "events_allowed": 10, "counter_in_plan": 10, "events_overage": 4,
                                    "counter_overage": 5, "overage_difference": 1})
        self.assertEqual(rows[u2]["overage_difference"], 3)


class Amounts(unittest.TestCase):
    def test_lines_round_half_up_once(self):
        self.assertEqual([billing.line_amount(u, 50) for u in (0, 9, 10, 2345, 1000)], [0, 0, 1, 117, 50])
        self.assertEqual(billing.line_amount(1, 500), 1)  # 0.5 cents rounds up
        self.assertEqual(billing.line_amount(1, 499), 0)

    def test_tokens_never_card_numbers(self):
        for bad in ("4242424242424242", "4242 4242 4242 4242", "4242-4242-4242-4242", "tok_378282246310005"):
            with self.assertRaises(payments.PaymentError):
                payments.check_token(bad)
        payments.check_token("tok_visa")


if __name__ == "__main__":
    unittest.main()
