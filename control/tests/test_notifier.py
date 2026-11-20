"""The alert notifier against the dev cluster and a local webhook receiver: an alert published on
rl-alerts (as lua/quota.lua does) reaches the merchant's webhook, signed; a server error is retried with
the same delivery id, a refusal is not; private addresses are not called unless allowed; alerts with no
merchant or no webhook go nowhere."""
import http.server
import json
import secrets
import socket
import threading
import time
import types
import unittest
from unittest import mock

from redis.exceptions import ConnectionError as RedisConnectionError
from redis.exceptions import RedisClusterException

from rlcontrol import store, webhooks
from rlcontrol.notifier import Notifier

from .test_api import cluster


class Receiver:
    """A webhook endpoint on 127.0.0.1 that records requests and answers with `statuses` in turn."""

    def __init__(self, statuses=(200,)):
        self.requests, self.statuses = [], list(statuses)
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                body = self.rfile.read(int(self.headers["Content-Length"]))
                outer.requests.append((dict(self.headers), body))
                code = outer.statuses.pop(0) if len(outer.statuses) > 1 else outer.statuses[0]
                self.send_response(code)
                self.end_headers()

            def log_message(self, *args):
                pass

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/hook"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def wait(self, n, timeout=5.0):
        end = time.time() + timeout
        while len(self.requests) < n and time.time() < end:
            time.sleep(0.02)
        return self.requests

    def close(self):
        self.server.shutdown()


class NotifierTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        try:
            cls.r = cluster()
            cls.r.ping()
        except (RedisClusterException, RedisConnectionError):
            raise unittest.SkipTest("Redis Cluster not reachable")

    def setUp(self):
        self.merchant = "n" + secrets.token_hex(5)
        self.secret = "whsec_" + secrets.token_hex(16)
        self.notifier = None

    def tearDown(self):
        if self.notifier:
            self.notifier.close()
        self.r.delete(store.webhook_key(self.merchant))

    def start(self, statuses=(200,), allow_private=True):
        receiver = Receiver(statuses)
        self.addCleanup(receiver.close)
        self.r.hset(store.webhook_key(self.merchant), mapping={"url": receiver.url, "secret": self.secret})
        self.notifier = Notifier(self.r, attempts=3, backoff_s=0.05, allow_private=allow_private)
        threading.Thread(target=self.notifier.run, daemon=True).start()
        time.sleep(0.5)  # subscribed
        return receiver

    def publish(self, customer=None, event="quota.80"):
        alert = {"customer": customer or f"{self.merchant}|u1", "event": event, "used": 8, "overage_used": 0,
                 "limit": 10, "period": "d20751", "at_ms": 1792900000000}
        self.r.publish(store.ALERTS, json.dumps(alert))

    def test_an_alert_reaches_the_merchants_webhook_signed(self):
        receiver = self.start()
        self.publish()
        (headers, body), = receiver.wait(1)
        payload = json.loads(body)
        self.assertEqual(payload, {"type": "quota.80", "merchant": self.merchant, "customer": "u1", "used": 8,
                                   "overage_used": 0, "limit": 10, "period": "d20751", "at_ms": 1792900000000})
        self.assertEqual(headers["X-RL-Event"], "quota.80")
        self.assertTrue(webhooks.verify(self.secret, headers["X-RL-Signature"], body))
        self.assertFalse(webhooks.verify("whsec_someone_else_entirely", headers["X-RL-Signature"], body))
        self.assertFalse(webhooks.verify(self.secret, headers["X-RL-Signature"], body, now=int(time.time()) + 3600))

    def test_a_server_error_is_retried_with_the_same_delivery_id(self):
        receiver = self.start(statuses=(500, 503, 200))
        self.publish()
        requests = receiver.wait(3)
        self.assertEqual(len(requests), 3)
        self.assertEqual(len({h["X-RL-Delivery"] for h, _ in requests}), 1)  # one delivery, three attempts
        time.sleep(0.1)
        self.assertEqual(self.notifier.delivered[-1][1], 200)

    def test_a_refusal_is_not_retried(self):
        receiver = self.start(statuses=(400,))
        self.publish()
        receiver.wait(1)
        time.sleep(0.4)
        self.assertEqual(len(receiver.requests), 1)

    def test_private_addresses_are_not_called_unless_allowed(self):
        receiver = self.start(allow_private=False)
        self.publish()
        time.sleep(1.0)
        self.assertEqual(receiver.requests, [])

    def test_alerts_without_a_merchant_or_a_webhook_go_nowhere(self):
        receiver = self.start()
        self.publish(customer="cust:single-owner")
        self.publish(customer="nobody-here|u1")
        time.sleep(0.8)
        self.assertEqual(receiver.requests, [])


class PinnedDeliveryTest(unittest.TestCase):
    """DNS rebinding: a name that answers a public address for the check and a private one afterwards.
    The delivery goes to the address checked, with the name in its Host header and as the server name
    its certificate is checked against; the name is resolved once."""

    def test_a_delivery_goes_to_the_address_checked(self):
        answers = [[(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("93.184.216.34", 8443))],
                   [(socket.AF_INET, socket.SOCK_STREAM, 6, "", ("127.0.0.1", 8443))]]
        sent = []

        class Client:
            def __init__(self, **_):
                pass

            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

            def post(self, url, content, headers, extensions):
                sent.append((url, headers["Host"], extensions))
                return types.SimpleNamespace(status_code=200)

        with mock.patch("socket.getaddrinfo", side_effect=lambda *a, **k: answers.pop(0)), mock.patch("httpx.Client", Client):
            notifier = Notifier(None, attempts=1, allow_private=False)
            status = notifier.send("https://hooks.example.com:8443/rl", "whsec_x", {"type": "quota.80"}, "d1")
            notifier.close()
        self.assertEqual(status, 200)
        self.assertEqual(sent, [("https://93.184.216.34:8443/rl", "hooks.example.com:8443",
                                 {"sni_hostname": "hooks.example.com"})])
        self.assertEqual(len(answers), 1)  # resolved once

    def test_an_ipv6_address_is_bracketed(self):
        self.assertEqual(webhooks.pinned("http://h.example/x", "2606:2800:220:1::1"),
                         ("http://[2606:2800:220:1::1]/x", {"Host": "h.example"}, {}))
        self.assertEqual(webhooks.pinned("http://h.example/x", None), ("http://h.example/x", {}, {}))


if __name__ == "__main__":
    unittest.main()
