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
import subprocess
import time
import unittest
from unittest import mock

from fastapi.testclient import TestClient
from redis.exceptions import ConnectionError as RedisConnectionError
from redis.exceptions import RedisClusterException

from rlcontrol import billing, payments, periods, store, webhooks
from rlcontrol.api import create_app

from .test_api import ADMIN, PLAN, cluster

ROOT = pathlib.Path(__file__).resolve().parents[2]
SECRET = "whsec-for-tests-only"  # the payment provider's webhook secret
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
        cls.client = TestClient(create_app(redis=cls.r, admin_token=ADMIN, provider=cls.provider, payment_secret=SECRET))
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
                self.r.hdel(payments.CHARGES, f"{m}/{iid}", *(f"{m}/{iid}/{n}" for n in range(2, 6)))
            for key in (store.catalog_key(m), store.invoices_key(m), store.purchases_key(m), store.payers_key(m),
                        store.settings_key(m), store.audit_key(m)):
                self.r.delete(key)
            for c in self.customers:
                self.r.delete(store.plan_key(m, c), store.quota_key(m, c))
            for h in (store.KEYS_HASH, store.ADMIN_HASH):
                mine = [k for k, owner in self.r.hgetall(h).items() if owner == m]
                if mine:
                    self.r.hdel(h, *mine)
            self.r.hdel(store.MERCHANTS, m)
        for event in getattr(self, "events", []):
            self.r.delete(store.payment_event_key(event))

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

    def test_a_customer_billed_from_its_subscription_date_is_closed_once_its_own_month_ends(self):  # P5
        m, auth = self.merchant()
        self.assertEqual(self.client.get("/v1/billing", headers=auth).json(), {"periods": "calendar"})
        self.client.put("/v1/billing", json={"periods": "subscription"}, headers=auth)
        self.customer(auth, "u1")
        day = self.client.get("/v1/customers/u1", headers=auth).json()["billing_day"]
        self.assertEqual(day, periods.anchor_at(store.now_ms(self.r)))  # the day it joined
        self.client.put("/v1/customers/u1/plan", json={"plan": "free"}, headers=auth)
        self.client.put("/v1/customers/u1/plan", json={"plan": "pay"}, headers=auth)
        self.assertEqual(self.client.get("/v1/customers/u1", headers=auth).json()["billing_day"], day)  # kept

        start, end = periods.month_bounds_ms(self.last, day)
        with mock.patch("rlcontrol.store.now_ms", return_value=end - 1):  # its month has not ended yet
            early = billing.close(self.r, self.last, merchants=[m])
        self.assertEqual((early["invoices"], early["not_ended"]), (0, 1))
        billing.close(self.r, self.last, merchants=[m])  # its day this month has come: today, or the 28th
        (inv,) = billing.invoices(self.r, m, self.last)
        self.assertEqual(inv["period"], {"from_ms": start, "to_ms": end})
        self.assertEqual(self.lines(inv), {"fee": (1, 1900)})

    def test_a_fee_changed_during_the_month_is_prorated(self):  # P5
        m, auth = self.merchant(plans=(("pay", PAY), ("free", PLAN), ("pro", {**PAY, "fee": 4900})))
        self.customer(auth, "u1")
        # Every change of the fee is recorded, by the API or tools/rlctl, and nothing else.
        self.client.put("/v1/customers/u1/plan", json={"plan": "pro"}, headers=auth)
        self.client.patch("/v1/customers/u1/limits", json={"daily": 7}, headers=auth)
        self.client.patch("/v1/customers/u1/limits", json={"fee": 2900}, headers=auth)
        tool = subprocess.run([str(ROOT / "tools/rlctl"), "--merchant", m, "--plan", "pay", "downgrade", "u1"],
                              capture_output=True, text=True)
        self.assertEqual(tool.returncode, 0, tool.stderr)
        now = store.now_ms(self.r)
        recorded = billing.fee_changes(self.r, m, now - 60_000, now + 1)["u1"]
        self.assertEqual([(before, after) for _, before, after in recorded], [(1900, 4900), (4900, 2900), (2900, 1900)])

        # Last month: 1900 for its first half, then 4900, the fee in force at close. A stream only grows, so
        # last month's change goes into an empty one first.
        self.r.delete(store.purchases_key(m))
        start, end = periods.month_bounds_ms(self.last)
        half = start + (end - start) // 2
        self.r.xadd(store.purchases_key(m), {"kind": "fee", "customer": "u1", "from": 1900, "to": 4900, "at_ms": half},
                    id=f"{half}-0")
        self.client.put("/v1/customers/u1/plan", json={"plan": "pro"}, headers=auth)  # recorded this month
        billing.close(self.r, self.last, merchants=[m])
        (inv,) = billing.invoices(self.r, m, self.last)
        (fee,) = inv["lines"]
        self.assertEqual(fee["amount"], 3400)  # (1900 + 4900) / 2
        self.assertEqual(fee["prorated"], [{"fee": 1900, "from_ms": start, "to_ms": half},
                                           {"fee": 4900, "from_ms": half, "to_ms": end}])

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

    def test_charges_once_and_a_declined_card_is_retried_then_suspends(self):
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

        now = store.now_ms(self.r)
        summary = billing.close(self.r, self.last, self.provider, [m])  # each owes last month's fee
        self.assertEqual((summary["paid"], summary["retrying"], summary["open"]), (1, 1, 1))
        u1, u2, u3 = billing.invoices(self.r, m, self.last)
        self.assertEqual((u1["status"], u1["payment"]["failure"]), ("paid", None))
        self.assertEqual((u2["status"], u2["payment"]["failure"], u2["attempts"]), ("retrying", "card_declined", 1))
        self.assertAlmostEqual(u2["next_attempt_ms"], now + 86_400_000, delta=60_000)  # tried again in a day
        self.assertEqual(u3["status"], "open")  # no card: for the merchant to collect
        self.assertEqual(self.r.hget(store.plan_key(m, "u2"), "plan"), "pay")  # not suspended on the first failure

        # u3 adds a card; closing again charges u3 only, and u1's charge is not repeated.
        self.client.put("/v1/customers/u3/payment-method", json={"token": "tok_mastercard"}, headers=auth)
        again = billing.close(self.r, self.last, self.provider, [m])
        self.assertEqual((again["invoices"], again["paid"], again["retrying"]), (0, 2, 1))
        charged = [k for k in self.r.hkeys(payments.CHARGES) if k.startswith(m + "/")]
        self.assertEqual(len(charged), 3)
        self.assertEqual(billing.invoices(self.r, m, self.last)[0]["payment"], u1["payment"])

        # The retries (P5): nothing before one is due; then after 1, 3 and 7 days, the last failure suspends.
        self.assertEqual(billing.retry(self.r, self.provider, [m]), {})
        for days, status in ((1, "retrying"), (1 + 3, "retrying"), (1 + 3 + 7, "payment_failed")):
            with mock.patch("rlcontrol.store.now_ms", return_value=now + days * 86_400_000 + 60_000):
                self.assertEqual(billing.retry(self.r, self.provider, [m]), {status: 1})
        u2 = billing.invoices(self.r, m, self.last)[1]
        self.assertEqual((u2["status"], u2["attempts"]), ("payment_failed", 4))
        self.assertEqual(self.r.hget(store.plan_key(m, "u2"), "plan"), "suspended")
        self.assertEqual(self.client.get("/v1/customers/u2", headers=auth).json()["suspended"], True)
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "plan"), "pay")

    def test_a_pending_charge_is_settled_by_the_providers_signed_webhook(self):  # P5
        m, auth = self.merchant()
        for cid in ("u1", "u2"):
            self.customer(auth, cid)
            self.client.put(f"/v1/customers/{cid}/payment-method", json={"token": "tok_pending"}, headers=auth)
        self.assertEqual(billing.close(self.r, self.last, self.provider, [m])["pending"], 2)
        u1, u2 = billing.invoices(self.r, m, self.last)
        hook = "/v1/payments/webhook"

        body, signature = self.provider.settle(f"{m}/{u1['id']}", ok=True, secret=SECRET)
        self.assertEqual(self.client.post(hook, content=body, headers={"X-Payment-Signature": signature}).json(),
                         {"invoice_status": "paid"})
        self.assertEqual(self.client.post(hook, content=body, headers={"X-Payment-Signature": signature}).json(),
                         {"duplicate": True})  # the provider sent it again: nothing done twice
        self.events = [json.loads(body)["id"]]
        for bad in ("", signature.replace("v1=", "v1=0"), webhooks.sign("another-secret", body)):
            self.assertEqual(self.client.post(hook, content=body, headers={"X-Payment-Signature": bad}).status_code, 400)
        stale = webhooks.sign(SECRET, body, at=int(time.time()) - 3600)  # replayed an hour later
        self.assertEqual(self.client.post(hook, content=body, headers={"X-Payment-Signature": stale}).status_code, 400)

        body, signature = self.provider.settle(f"{m}/{u2['id']}", ok=False, secret=SECRET)
        self.events.append(json.loads(body)["id"])
        self.assertEqual(self.client.post(hook, content=body, headers={"X-Payment-Signature": signature}).json(),
                         {"invoice_status": "retrying"})  # a failed settlement is retried as a declined charge is
        self.assertEqual([i["status"] for i in billing.invoices(self.r, m, self.last)], ["paid", "retrying"])

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
