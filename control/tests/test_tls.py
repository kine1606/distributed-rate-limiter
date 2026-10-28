"""TLS on the limiter (src/server/main.cc, RL_TLS_CERT and RL_TLS_KEY): with a certificate, a TLS client
gets answers and a plain-text one is refused; a half-set or unreadable pair stops the start. Runs
build/release/rate_limiter_server with the allow_all engine and a self-signed certificate made by
openssl for this test."""
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

import grpc
from grpc_tools import protoc

ROOT = pathlib.Path(__file__).resolve().parents[2]
SERVER = ROOT / "build/release/rate_limiter_server"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class TlsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SERVER.exists() or shutil.which("openssl") is None:
            raise unittest.SkipTest("needs build/release/rate_limiter_server and openssl")
        cls.dir = pathlib.Path(tempfile.mkdtemp())
        cls.cert, cls.key = cls.dir / "cert.pem", cls.dir / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256", "-nodes",
                        "-days", "1", "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
                        "-keyout", str(cls.key), "-out", str(cls.cert)], check=True, capture_output=True)
        assert protoc.main(["protoc", f"-I{ROOT / 'proto'}", f"--python_out={cls.dir}", f"--grpc_python_out={cls.dir}",
                            str(ROOT / "proto/rate_limiter.proto")]) == 0
        sys.path.insert(0, str(cls.dir))
        import rate_limiter_pb2
        import rate_limiter_pb2_grpc
        cls.pb, cls.rpc = rate_limiter_pb2, rate_limiter_pb2_grpc

    def start(self, **env):
        port = free_port()
        proc = subprocess.Popen([str(SERVER), f"127.0.0.1:{port}", str(ROOT / "config/rules.yaml"), "127.0.0.1:7001",
                                 str(ROOT / "lua"), "allow_all"],
                                env={**os.environ, "RL_METRICS_ADDR": "off", "RL_REDIS_LOOPS": "1", **env},
                                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(lambda: (proc.terminate(), proc.wait(timeout=10)))
        return proc, port

    def wait_listening(self, port):
        for _ in range(100):
            with socket.socket() as s:
                if s.connect_ex(("127.0.0.1", port)) == 0:
                    return
            time.sleep(0.1)
        self.fail("the limiter did not start")

    def check(self, channel):
        with channel:
            return self.rpc.RateLimiterStub(channel).Check(self.pb.CheckRequest(key="k", rule="api_default"), timeout=3)

    def test_with_a_certificate_only_tls_clients_are_answered(self):
        _, port = self.start(RL_TLS_CERT=str(self.cert), RL_TLS_KEY=str(self.key))
        self.wait_listening(port)
        tls = grpc.ssl_channel_credentials(root_certificates=self.cert.read_bytes())
        self.assertTrue(self.check(grpc.secure_channel(f"localhost:{port}", tls)).allowed)
        with self.assertRaises(grpc.RpcError) as refused:
            self.check(grpc.insecure_channel(f"localhost:{port}"))
        self.assertEqual(refused.exception.code(), grpc.StatusCode.UNAVAILABLE)

    def test_a_half_set_or_unreadable_pair_stops_the_start(self):
        for env in ({"RL_TLS_CERT": str(self.cert)}, {"RL_TLS_CERT": str(self.cert), "RL_TLS_KEY": "/nonexistent.pem"}):
            proc, _ = self.start(**env)
            self.assertEqual(proc.wait(timeout=10), 1, env)


if __name__ == "__main__":
    unittest.main()
