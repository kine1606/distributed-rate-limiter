"""The P2 gate, end to end (docs/PRODUCT_PLAN.md): the real limiter (build/release/rate_limiter_server,
RL_TENANCY=merchants), the management API, the notifier and a webhook receiver, on the dev cluster.

1. The advisor's three answers at the quota, one plan each, through gRPC: block (hard), a bounded free
   grace, and postpaid overage up to a spending cap.
2. Changed live, per customer, through the API: a blocked customer switched to postpaid, a boost, a
   prepaid top-up; each reaches the limiter at once, past the denial it holds locally.
3. The alerts of all of that reach the merchant's webhook, signed.
4. Another merchant's customer with the same id is someone else; a wrong key is refused; revoking the
   merchant's check keys locks its gateway out."""
import json
import os
import pathlib
import secrets
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

import grpc
from fastapi.testclient import TestClient
from redis.exceptions import ConnectionError as RedisConnectionError
from redis.exceptions import RedisClusterException

from rlcontrol import store, webhooks
from rlcontrol.api import create_app
from rlcontrol.notifier import Notifier

from .test_api import ADMIN, cluster
from .test_notifier import Receiver

ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER = ROOT / "build/release/rate_limiter_server"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class GateTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.r = cluster()
            cls.r.ping()
        except (RedisClusterException, RedisConnectionError):
            raise unittest.SkipTest("Redis Cluster not reachable")
        if not SERVER.exists():
            raise unittest.SkipTest("build/release/rate_limiter_server not built")
        cls.gen = tempfile.mkdtemp()
        from grpc_tools import protoc
        assert protoc.main(["protoc", f"-I{ROOT / 'proto'}", f"--python_out={cls.gen}", f"--grpc_python_out={cls.gen}",
                            str(ROOT / "proto/rate_limiter.proto")]) == 0
        sys.path.insert(0, cls.gen)
        import rate_limiter_pb2
        import rate_limiter_pb2_grpc
        cls.pb, cls.rpc = rate_limiter_pb2, rate_limiter_pb2_grpc

        port = free_port()
        env = {**os.environ, "RL_TENANCY": "merchants", "RL_METRICS_ADDR": "off"}
        seed = os.environ.get("RL_TEST_CLUSTER", "127.0.0.1:7001")
        cls.server = subprocess.Popen([str(SERVER), f"127.0.0.1:{port}", str(ROOT / "config/rules.yaml"), seed,
                                       str(ROOT / "lua"), "leased"], env=env, stdout=subprocess.DEVNULL,
                                      stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=0.1).close()
                break
            except OSError:
                time.sleep(0.1)
        cls.channel = grpc.insecure_channel(f"127.0.0.1:{port}")
        cls.stub = cls.rpc.RateLimiterStub(cls.channel)
        cls.api = TestClient(create_app(redis=cls.r, admin_token=ADMIN))

    @classmethod
    def tearDownClass(cls):
        cls.channel.close()
        cls.server.terminate()
        cls.server.wait(timeout=10)

    def setUp(self):
        self.merchants = []

    def tearDown(self):
        for m in self.merchants:
            for key in (store.catalog_key(m), store.webhook_key(m), store.purchases_key(m)):
                self.r.delete(key)
            for c in ("hard1", "grace1", "post1"):
                self.r.delete(store.plan_key(m, c), store.quota_key(m, c))
            for h in (store.KEYS_HASH, store.ADMIN_HASH):
                mine = [k for k, owner in self.r.hgetall(h).items() if owner == m]
                if mine:
                    self.r.hdel(h, *mine)
            self.r.hdel(store.MERCHANTS, m)

    def merchant(self):
        m = "g" + secrets.token_hex(5)
        body = self.api.post("/v1/merchants", json={"id": m}, headers={"Authorization": f"Bearer {ADMIN}"}).json()
        self.merchants.append(m)
        return m, {"Authorization": f"Bearer {body['management_key']}"}, body["check_key"]

    def check(self, key, customer):
        try:
            return self.stub.Check(self.pb.CheckRequest(key=customer, rule="customer"), metadata=[("x-api-key", key)],
                                   timeout=2)
        except grpc.RpcError as e:
            return e.code()

    def reason(self, key, customer):
        r = self.check(key, customer)
        return r if isinstance(r, grpc.StatusCode) else self.pb.Reason.Name(r.reason)

    def until(self, key, customer, want, timeout=1.0):
        """Calls until the answer is `want` (a change on its way to the limiter), or gives up."""
        end = time.time() + timeout
        got = None
        while time.time() < end:
            got = self.reason(key, customer)
            if got == want:
                return got
            time.sleep(0.02)
        return got

    def test_the_three_answers_changed_live_with_alerts(self):
        m, auth, key = self.merchant()
        receiver = Receiver()
        self.addCleanup(receiver.close)
        with mock.patch.dict(os.environ, {"RL_ALLOW_PRIVATE_WEBHOOKS": "1"}):
            secret = self.api.put("/v1/webhook", json={"url": receiver.url}, headers=auth).json()["secret"]
        notifier = Notifier(self.r, backoff_s=0.05, allow_private=True)
        threading.Thread(target=notifier.run, daemon=True).start()
        self.addCleanup(notifier.close)
        time.sleep(0.5)

        base = {"daily": 10, "burst": 1000, "refill": 1000}
        for name, terms in (("strict", {"overage": "hard"}), ("cushion", {"overage": "grace", "allowance": 1}),
                            ("payg", {"overage": "postpaid", "allowance": 3})):
            self.assertEqual(self.api.put(f"/v1/plans/{name}", json={**base, **terms}, headers=auth).status_code, 200)
        for cid, plan in (("hard1", "strict"), ("grace1", "cushion"), ("post1", "payg")):
            self.assertEqual(self.api.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth).status_code, 201)
        self.until(key, "hard1", "REASON_OK", timeout=2.0)  # the limiter has loaded the merchant's key
        self.r.delete(store.quota_key(m, "hard1"))

        # 1. The three answers.
        self.assertEqual([self.reason(key, "hard1") for _ in range(11)], ["REASON_OK"] * 10 + ["REASON_QUOTA_EXHAUSTED"])
        self.assertEqual([self.reason(key, "grace1") for _ in range(12)], ["REASON_OK"] * 11 + ["REASON_QUOTA_EXHAUSTED"])
        self.assertEqual([self.reason(key, "post1") for _ in range(14)],
                         ["REASON_OK"] * 10 + ["REASON_OVERAGE"] * 3 + ["REASON_QUOTA_EXHAUSTED"])

        # 2. Changed live, per customer, past the denials each instance now holds.
        self.api.patch("/v1/customers/hard1/limits", json={"overage": "postpaid", "allowance": 2}, headers=auth)
        self.assertEqual(self.until(key, "hard1", "REASON_OVERAGE"), "REASON_OVERAGE")
        self.assertEqual(self.reason(key, "hard1"), "REASON_OVERAGE")
        self.assertEqual(self.reason(key, "hard1"), "REASON_QUOTA_EXHAUSTED")  # the cap of 2
        self.api.post("/v1/customers/grace1/boost", json={"units": 2, "minutes": 5}, headers=auth)
        self.assertEqual(self.until(key, "grace1", "REASON_OK"), "REASON_OK")
        self.assertEqual(self.reason(key, "grace1"), "REASON_OK")
        self.assertEqual(self.reason(key, "grace1"), "REASON_QUOTA_EXHAUSTED")
        self.api.post("/v1/customers/post1/topups", json={"units": 2}, headers=auth)
        self.assertEqual(self.until(key, "post1", "REASON_OK"), "REASON_OK")
        self.assertEqual(self.reason(key, "post1"), "REASON_OK")
        self.assertEqual(self.reason(key, "post1"), "REASON_QUOTA_EXHAUSTED")
        usage = self.api.get("/v1/customers/post1", headers=auth).json()["usage"]
        self.assertEqual((usage["used"], usage["overage_used"], usage["limit"]), (12, 3, 12))

        # 3. The alerts, signed, for the right customers.
        expected = {("hard1", "quota.80"), ("hard1", "quota.100"), ("hard1", "quota.denied"), ("hard1", "overage.started"),
                    ("grace1", "quota.80"), ("grace1", "quota.100"), ("grace1", "quota.denied"),
                    ("post1", "quota.80"), ("post1", "quota.100"), ("post1", "overage.started"), ("post1", "quota.denied")}
        end = time.time() + 5
        while len(receiver.requests) < len(expected) and time.time() < end:
            time.sleep(0.05)
        got = set()
        for headers, body in receiver.requests:
            self.assertTrue(webhooks.verify(secret, headers["X-RL-Signature"], body))
            p = json.loads(body)
            self.assertEqual(p["merchant"], m)
            got.add((p["customer"], p["type"]))
        self.assertEqual(got, expected)

        # 4. Isolation and keys.
        other, other_auth, other_key = self.merchant()
        self.api.put("/v1/plans/strict", json={**base, "overage": "hard"}, headers=other_auth)
        self.api.post("/v1/customers", json={"id": "hard1", "plan": "strict"}, headers=other_auth)
        self.assertEqual(self.until(other_key, "hard1", "REASON_OK", timeout=2.0), "REASON_OK")  # its own hard1
        self.assertEqual(self.reason("rl_" + "0" * 48, "hard1"), grpc.StatusCode.UNAUTHENTICATED)
        self.api.delete(f"/v1/merchants/{m}/check-keys", headers={"Authorization": f"Bearer {ADMIN}"})
        self.assertEqual(self.until(key, "hard1", grpc.StatusCode.UNAUTHENTICATED), grpc.StatusCode.UNAUTHENTICATED)


if __name__ == "__main__":
    unittest.main()
