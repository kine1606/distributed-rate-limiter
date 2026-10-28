"""The management API against the dev cluster (127.0.0.1:7001, RL_TEST_CLUSTER): credentials, isolation
between merchants, validation, plans, customers, custom limits, boosts, top-ups (idempotent), suspension,
webhooks, and the usage it reports against what lua/quota.lua counted. Every merchant is new (random id)
and every key it wrote is deleted afterwards."""
import os
import pathlib
import secrets
import unittest
from unittest import mock

from fastapi.testclient import TestClient
from redis.cluster import RedisCluster
from redis.exceptions import RedisClusterException, ConnectionError as RedisConnectionError

from rlcontrol import periods, store
from rlcontrol.api import create_app

ROOT = pathlib.Path(__file__).resolve().parents[2]
ADMIN = "operator-token-for-tests"
PLAN = {"daily": 10, "burst": 100, "refill": 100, "overage": "hard"}


def cluster():
    host, _, port = os.environ.get("RL_TEST_CLUSTER", "127.0.0.1:7001").partition(":")
    return RedisCluster(host=host, port=int(port or 7001), decode_responses=True)


class ApiTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.r = cluster()
            cls.r.ping()
        except (RedisClusterException, RedisConnectionError):
            raise unittest.SkipTest("Redis Cluster not reachable")
        cls.client = TestClient(create_app(redis=cls.r, admin_token=ADMIN))

    def setUp(self):
        self.merchants = []

    def tearDown(self):
        for m in self.merchants:
            for key in (store.catalog_key(m), store.webhook_key(m), store.purchases_key(m), store.customers_key(m)):
                self.r.delete(key)
            for c in ("u1", "u2", "u3", "u4", "u5", "ghost"):
                self.r.delete(store.plan_key(m, c))
                self.r.delete(store.quota_key(m, c))
            for h in (store.KEYS_HASH, store.ADMIN_HASH):
                mine = [k for k, owner in self.r.hgetall(h).items() if owner == m]
                if mine:
                    self.r.hdel(h, *mine)
            self.r.hdel(store.MERCHANTS, m)
            self.r.hdel(store.CAPACITY_HASH, m)
            self.r.hdel(store.CHECK_RATE_HASH, m)
        for key in getattr(self, "idem_keys", []):
            self.r.delete(key)

    def op(self):
        return {"Authorization": f"Bearer {ADMIN}"}

    def new_merchant(self):
        m = "t" + secrets.token_hex(5)
        res = self.client.post("/v1/merchants", json={"id": m}, headers=self.op())
        self.assertEqual(res.status_code, 201, res.text)
        self.merchants.append(m)
        body = res.json()
        return m, {"Authorization": f"Bearer {body['management_key']}"}, body

    def with_plan(self, auth, name="basic", **terms):
        res = self.client.put(f"/v1/plans/{name}", json={**PLAN, **terms}, headers=auth)
        self.assertEqual(res.status_code, 200, res.text)

    def with_customer(self, auth, cid="u1", plan="basic"):
        res = self.client.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth)
        self.assertEqual(res.status_code, 201, res.text)
        return res.json()

    # ---- credentials ----

    def test_only_the_operator_creates_merchants(self):
        self.assertEqual(self.client.post("/v1/merchants", json={"id": "x"}).status_code, 401)
        self.assertEqual(self.client.post("/v1/merchants", json={"id": "x"},
                                          headers={"Authorization": "Bearer wrong-token-wrong"}).status_code, 401)
        m, _, body = self.new_merchant()
        self.assertTrue(body["check_key"].startswith("rl_") and body["management_key"].startswith("rlm_"))
        self.assertEqual(self.r.hget(store.KEYS_HASH, store.sha256_hex(body["check_key"])), m)  # as the limiter reads it
        self.assertEqual(self.r.hget(store.ADMIN_HASH, store.sha256_hex(body["management_key"])), m)
        self.assertEqual(self.client.post("/v1/merchants", json={"id": m}, headers=self.op()).status_code, 409)
        self.assertEqual(self.client.post("/v1/merchants", json={"id": "bad|id"}, headers=self.op()).status_code, 422)

    def test_a_merchant_sees_only_its_own_customers(self):
        a, auth_a, keys_a = self.new_merchant()
        b, auth_b, _ = self.new_merchant()
        self.with_plan(auth_a)
        self.with_customer(auth_a)
        self.assertEqual(self.client.get("/v1/customers/u1", headers=auth_b).status_code, 404)  # B has no u1
        self.assertEqual(self.client.get("/v1/customers/u1", headers=auth_a).status_code, 200)
        self.assertEqual(self.client.get("/v1/customers/u1").status_code, 401)
        gateway_key = {"Authorization": f"Bearer {keys_a['check_key']}"}
        self.assertEqual(self.client.get("/v1/customers/u1", headers=gateway_key).status_code, 401)  # not a management key
        self.assertEqual(self.client.get("/v1/plans", headers=auth_b).json(), {})
        self.assertFalse(self.r.exists(store.plan_key(b, "u1")))

    def test_check_keys_rotate_and_revoke(self):
        m, _, first = self.new_merchant()
        res = self.client.post(f"/v1/merchants/{m}/check-keys", headers=self.op())
        self.assertEqual(res.status_code, 201)
        second = res.json()["check_key"]
        self.assertEqual(self.r.hget(store.KEYS_HASH, store.sha256_hex(second)), m)
        res = self.client.delete(f"/v1/merchants/{m}/check-keys", headers=self.op())
        self.assertEqual(res.json()["revoked"], 2)
        for key in (first["check_key"], second):
            self.assertIsNone(self.r.hget(store.KEYS_HASH, store.sha256_hex(key)))

    # ---- plans ----

    def test_plans_are_validated_and_listed(self):
        _, auth, _ = self.new_merchant()
        bad = [("Bad", PLAN), ("suspended", PLAN), ("ok", {**PLAN, "overage": "unlimited"}), ("ok", {**PLAN, "daily": -1}),
               ("ok", {**PLAN, "daily": 2 ** 32}), ("ok", {**PLAN, "colour": "red"}), ("ok", {"daily": 5}),
               ("ok", {**PLAN, "fee": -1}), ("ok", {**PLAN, "overage_price": 1.5})]
        for name, body in bad:
            self.assertEqual(self.client.put(f"/v1/plans/{name}", json=body, headers=auth).status_code, 422, (name, body))
        self.with_plan(auth, "pro", daily=5000, period="month", overage="postpaid", allowance=500, failure="open",
                       fee=1900, overage_price=50, topup_price=40)
        listed = self.client.get("/v1/plans", headers=auth).json()
        self.assertEqual(listed["pro"], {"daily": 5000, "burst": 100, "refill": 100, "period": "month",
                                         "overage": "postpaid", "allowance": 500, "failure": "open",
                                         "fee": 1900, "overage_price": 50, "topup_price": 40})
        self.assertEqual(self.client.delete("/v1/plans/pro", headers=auth).status_code, 204)
        self.assertEqual(self.client.delete("/v1/plans/pro", headers=auth).status_code, 404)

    # ---- customers ----

    def test_customer_lifecycle_and_custom_limits(self):
        m, auth, _ = self.new_merchant()
        self.assertEqual(self.client.post("/v1/customers", json={"id": "u1", "plan": "nosuch"}, headers=auth).status_code, 404)
        self.with_plan(auth, overage="grace", allowance=1)
        view = self.with_customer(auth)
        self.assertEqual(view["terms"]["overage"], "grace")
        self.assertEqual(view["usage"]["used"], 0)
        self.assertEqual(view["usage"]["limit"], 10)
        self.assertEqual(self.client.post("/v1/customers", json={"id": "u1", "plan": "basic"}, headers=auth).status_code, 409)
        self.assertEqual(self.client.post("/v1/customers", json={"id": "a b", "plan": "basic"}, headers=auth).status_code, 422)

        sub = self.r.pubsub()
        sub.subscribe(store.CHANNEL)
        sub.get_message(timeout=1)
        res = self.client.patch("/v1/customers/u1/limits", json={"overage": "postpaid", "allowance": 50}, headers=auth)
        self.assertEqual(res.status_code, 200, res.text)
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "overage"), "cap")  # what the limiter reads
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "extra"), "50")
        announced = None
        while announced is None:
            msg = sub.get_message(timeout=2)
            self.assertIsNotNone(msg, "the change was not announced")
            if msg["type"] == "message":
                announced = msg["data"]
        self.assertEqual(announced, f"{m}|u1")
        sub.close()
        self.assertEqual(self.client.patch("/v1/customers/u1/limits", json={}, headers=auth).status_code, 422)
        self.assertEqual(self.client.patch("/v1/customers/ghost/limits", json={"daily": 5}, headers=auth).status_code, 404)

    def test_boosts_and_idempotent_top_ups(self):
        m, auth, _ = self.new_merchant()
        self.with_plan(auth)
        self.with_plan(auth, "bigger", daily=20)
        self.with_customer(auth)
        view = self.client.post("/v1/customers/u1/boost", json={"units": 5, "minutes": 10}, headers=auth).json()
        self.assertTrue(view["boost"]["active"])
        self.assertAlmostEqual(view["boost"]["until_ms"], store.now_ms(self.r) + 600_000, delta=5_000)
        self.assertEqual(view["usage"]["limit"], 15)
        view = self.client.delete("/v1/customers/u1/boost", headers=auth).json()
        self.assertFalse(view["boost"]["active"])
        self.assertEqual(view["usage"]["limit"], 10)

        key = "topup-" + secrets.token_hex(4)
        self.idem_keys = [store.idempotency_key(m, key)]
        first = self.client.post("/v1/customers/u1/topups", json={"units": 3}, headers={**auth, "Idempotency-Key": key})
        self.assertEqual(first.status_code, 201, first.text)
        self.assertEqual(first.json()["period_total"], 3)
        again = self.client.post("/v1/customers/u1/topups", json={"units": 3}, headers={**auth, "Idempotency-Key": key})
        self.assertEqual(again.json(), first.json())  # the retry bought nothing
        self.assertEqual(again.headers.get("Idempotent-Replayed"), "true")
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "topup"), "3")
        self.assertEqual(self.client.post("/v1/customers/u1/topups", json={"units": 2}, headers=auth).json()["period_total"], 5)
        self.assertEqual(self.r.xlen(store.purchases_key(m)), 2)  # two purchases for the invoice
        today = periods.period_name(store.now_ms(self.r), False)
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "topup_period"), today)

        view = self.client.put("/v1/customers/u1/plan", json={"plan": "bigger"}, headers=auth).json()
        self.assertEqual(view["terms"]["daily"], 20)
        self.assertEqual(view["topup"]["units"], 5)  # a plan change keeps what was paid for
        self.assertEqual(view["usage"]["limit"], 25)
        self.assertEqual(self.client.post("/v1/customers/ghost/topups", json={"units": 1}, headers=auth).status_code, 404)
        self.assertEqual(self.client.post("/v1/customers/u1/topups", json={"units": 0}, headers=auth).status_code, 422)

    def test_suspension_keeps_the_numbers(self):
        m, auth, _ = self.new_merchant()
        self.with_plan(auth)
        self.with_customer(auth)
        self.client.patch("/v1/customers/u1/limits", json={"daily": 77}, headers=auth)
        view = self.client.post("/v1/customers/u1/suspend", json={}, headers=auth).json()
        self.assertTrue(view["suspended"])
        self.assertEqual(self.r.hget(store.plan_key(m, "u1"), "plan"), "suspended")  # what the limiter reads
        view = self.client.post("/v1/customers/u1/resume", json={}, headers=auth).json()
        self.assertFalse(view["suspended"])
        self.assertEqual(view["plan"], "basic")
        self.assertEqual(view["terms"]["daily"], 77)

    # ---- capacity and check rate (P3) ----

    def test_a_merchant_sets_its_capacity_and_the_operator_its_check_rate(self):
        m, auth, _ = self.new_merchant()
        for bad in ({"rate": 0}, {"rate": 10, "reserve": 101}, {"rate": 10, "burst": 0}, {"rate": 10, "colour": 1},
                    {"rate": 1, "reserve": 30}):  # a burst of 1 leaves Free calls no whole call: the limiter would refuse all
            self.assertEqual(self.client.put("/v1/capacity", json=bad, headers=auth).status_code, 422, bad)
        self.assertEqual(self.client.get("/v1/capacity", headers=auth).status_code, 404)
        pubsub = self.r.pubsub()
        pubsub.subscribe(store.CHANNEL)
        pubsub.get_message(timeout=1)  # the subscription's confirmation
        res = self.client.put("/v1/capacity", json={"rate": 1000, "reserve": 20}, headers=auth)
        self.assertEqual(res.json(), {"rate": 1000, "burst": 1000, "reserve": 20})
        self.assertEqual(self.r.hget(store.CAPACITY_HASH, m), "rate 1000 burst 1000 reserve 20")  # as the limiter reads it
        message = pubsub.get_message(timeout=2)
        self.assertEqual(message and message["data"], store.MERCHANTS_CHANGED)  # every limiter reloads
        pubsub.close()
        self.assertEqual(self.client.get("/v1/capacity", headers=auth).json()["reserve"], 20)

        path = f"/v1/merchants/{m}/check-rate"
        self.assertEqual(self.client.put(path, json={"rate": 5000}, headers=auth).status_code, 401)  # not a merchant's to set
        self.assertEqual(self.client.put("/v1/merchants/nobody-here/check-rate", json={"rate": 5}, headers=self.op()).status_code, 404)
        self.assertEqual(self.client.put(path, json={"rate": 5000, "burst": 8000}, headers=self.op()).json(),
                         {"id": m, "rate": 5000, "burst": 8000})
        self.assertEqual(self.r.hget(store.CHECK_RATE_HASH, m), "rate 5000 burst 8000")
        self.assertEqual(self.client.delete(path, headers=self.op()).status_code, 204)
        self.assertEqual(self.client.get(path, headers=self.op()).status_code, 404)
        self.assertEqual(self.client.delete("/v1/capacity", headers=auth).status_code, 204)
        self.assertIsNone(self.r.hget(store.CAPACITY_HASH, m))

    # ---- the customer list, usage and the portal (the portal's prerequisites) ----

    def test_customers_are_listed_a_page_at_a_time_and_only_ones_own(self):
        _, auth, _ = self.new_merchant()
        _, other_auth, _ = self.new_merchant()
        self.with_plan(auth)
        for cid in ("u3", "u1", "u5", "u2", "u4"):
            self.with_customer(auth, cid)
        seen, after = [], ""
        while True:
            page = self.client.get(f"/v1/customers?limit=2&after={after}", headers=auth).json()
            seen.append([c["id"] for c in page["customers"]])
            if page["next"] is None:
                break
            after = page["next"]
        self.assertEqual(seen, [["u1", "u2"], ["u3", "u4"], ["u5"]])  # id order, every one once
        self.assertEqual(self.client.get("/v1/customers?limit=1", headers=auth).json()["customers"][0]["usage"]["limit"], 10)
        self.assertEqual(self.client.get("/v1/customers", headers=other_auth).json(), {"customers": [], "next": None})
        for bad in ("after=a%7Cb", "limit=0", "limit=201"):
            self.assertEqual(self.client.get(f"/v1/customers?{bad}", headers=auth).status_code, 422, bad)

    def test_a_customer_missing_from_the_index_comes_back(self):
        m, auth, _ = self.new_merchant()
        self.with_plan(auth)
        self.with_customer(auth, "u1")
        self.r.hset(store.plan_key(m, "u2"), mapping={"plan": "basic", "daily": 10})  # as tools/rlctl wrote it before
        self.r.delete(store.customers_key(m))  # as if a crash fell between the two writes
        listed = lambda: [c["id"] for c in self.client.get("/v1/customers", headers=auth).json()["customers"]]
        self.assertEqual(listed(), [])
        self.assertEqual(self.client.post("/v1/customers/reindex", headers=auth).status_code, 401)  # the operator's
        self.assertEqual(self.client.post("/v1/customers/reindex", headers=self.op()).json()[m], 2)
        self.assertEqual(listed(), ["u1", "u2"])
        self.r.delete(store.customers_key(m))
        self.assertEqual(self.client.post("/v1/customers", json={"id": "u1", "plan": "basic"}, headers=auth).status_code, 409)
        self.assertEqual(listed(), ["u1"])  # creating it again puts it back too

    def test_usage_adds_up_what_the_script_counted_this_month(self):
        m, auth, _ = self.new_merchant()
        self.with_plan(auth, overage="postpaid", allowance=100)
        quota = self.r.register_script((ROOT / "lua/quota.lua").read_text())
        for cid, calls in (("u1", 12), ("u2", 3)):  # u1: 10 in the plan, 2 billed
            self.with_customer(auth, cid)
            for _ in range(calls):
                quota(keys=[store.quota_key(m, cid)], args=[10, 1, "cap", 100, "day", 0, 0, 0, ""])
        u = self.client.get("/v1/usage", headers=auth).json()
        self.assertEqual((u["customers"], u["admitted"], u["billed_overage"]), (2, 15, 2))
        self.assertEqual(u["month"], periods.month_label(periods.month_of(store.now_ms(self.r))))
        self.assertGreater(u["at_ms"], 0)

    def test_the_portal_is_served_with_a_strict_policy(self):
        page = self.client.get("/portal")
        self.assertEqual((page.status_code, page.headers["content-type"]), (200, "text/html; charset=utf-8"))
        self.assertIn("default-src 'none'", page.headers["content-security-policy"])
        self.assertIn('<script src="/portal/portal.js">', page.text)
        self.assertEqual(self.client.get("/portal/portal.js").headers["content-type"].split(";")[0], "text/javascript")
        for name in ("api.py", "store.py", "..%2Fapi.py"):
            self.assertEqual(self.client.get(f"/portal/{name}").status_code, 404, name)

    def test_webhooks_refuse_unsafe_urls(self):
        _, auth, _ = self.new_merchant()
        for url in ("http://127.0.0.1:9/hook", "http://169.254.169.254/latest", "ftp://example.org/x",
                    "https://user:pw@example.org/x", "not a url"):
            res = self.client.put("/v1/webhook", json={"url": url}, headers=auth)
            self.assertEqual(res.status_code, 422, url)
        with mock.patch.dict(os.environ, {"RL_ALLOW_PRIVATE_WEBHOOKS": "1"}):
            res = self.client.put("/v1/webhook", json={"url": "http://127.0.0.1:9/hook"}, headers=auth)
        self.assertEqual(res.status_code, 200, res.text)
        self.assertTrue(res.json()["secret"].startswith("whsec_"))
        self.assertEqual(self.client.get("/v1/webhook", headers=auth).json(), {"url": "http://127.0.0.1:9/hook"})
        self.assertEqual(self.client.delete("/v1/webhook", headers=auth).status_code, 204)
        self.assertEqual(self.client.get("/v1/webhook", headers=auth).status_code, 404)

    # ---- usage, as the script counts it ----

    def test_usage_is_what_the_script_counted(self):
        m, auth, _ = self.new_merchant()
        self.with_plan(auth, daily=5, overage="postpaid", allowance=2)
        self.with_customer(auth)
        quota = self.r.register_script((ROOT / "lua/quota.lua").read_text())
        for _ in range(6):  # 5 in the plan, 1 billed
            quota(keys=[store.quota_key(m, "u1")], args=[5, 1, "cap", 2, "day"])
        usage = self.client.get("/v1/customers/u1", headers=auth).json()["usage"]
        self.assertEqual((usage["used"], usage["overage_used"], usage["remaining"]), (5, 1, 0))
        self.assertEqual(usage["alerts"], ["quota.80", "quota.100", "overage.started"])
        self.assertEqual(usage["period"], periods.period_name(store.now_ms(self.r), False))


if __name__ == "__main__":
    unittest.main()
