"""The example gateway (examples/gateway-python/gateway.py): whom it limits, on a fake limiter. A user is
named by an API key the gateway knows, never by a header the client sets; an unknown key is refused
before the limiter is asked; a limiter error does not reach the client."""
import http.server
import importlib.util
import json
import pathlib
import threading
import unittest
import urllib.error
import urllib.request

import grpc

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("gateway", ROOT / "examples/gateway-python/gateway.py")
gateway = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gateway)


class LimiterDown(grpc.RpcError):
    def code(self):
        return grpc.StatusCode.UNAVAILABLE

    def details(self):
        return "failed to connect to 10.0.0.7:50051"


class FakeLimiter:
    def __init__(self):
        self.keys, self.down = [], False

    def Check(self, request, timeout):
        if self.down:
            raise LimiterDown()
        self.keys.append(request.key)
        return gateway.pb.CheckResponse(allowed=True, limit=5, remaining=4)


class GatewayTest(unittest.TestCase):
    def setUp(self):
        self.limiter = FakeLimiter()
        gateway.Gateway.limiter = self.limiter
        gateway.Gateway.log_message = lambda *a: None  # quiet test output
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), gateway.Gateway)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.shutdown)

    def get(self, headers):
        req = urllib.request.Request(f"http://127.0.0.1:{self.server.server_address[1]}/api/hello", headers=headers)
        try:
            with urllib.request.urlopen(req) as r:
                return r.status, json.loads(r.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def test_a_user_is_named_by_its_api_key_never_by_a_header(self):
        self.assertEqual(self.get({"X-Api-Key": "demo-alice"})[0], 200)
        self.assertEqual(self.get({"X-User-Id": "alice"})[0], 200)  # no key: limited by its address
        self.assertEqual(self.limiter.keys, ["user:alice", "ip:127.0.0.1"])

    def test_an_unknown_key_is_refused_before_the_limiter(self):
        self.assertEqual(self.get({"X-Api-Key": "guessed"}), (401, {"error": "unknown API key"}))
        self.assertEqual(self.limiter.keys, [])

    def test_a_limiter_error_does_not_reach_the_client(self):
        self.limiter.down = True
        status, body = self.get({"X-Api-Key": "demo-alice"})
        self.assertEqual((status, body), (503, {"error": "rate limiter unavailable"}))


if __name__ == "__main__":
    unittest.main()
