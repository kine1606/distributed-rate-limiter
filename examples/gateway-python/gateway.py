#!/usr/bin/env python3
"""Minimal API gateway: every HTTP request asks the rate limiter (gRPC Check) before it is served.

    client --HTTP--> gateway --gRPC Check--> rate_limiter_server --EVALSHA--> Redis (token_bucket.lua)

Allowed -> 200 from a stub upstream, with X-RateLimit-* headers.
Denied  -> 429 with Retry-After.
Limiter unreachable or Redis down on a fail_closed rule -> 503.

Usage: gateway.py [--listen 0.0.0.0:8080] [--limiter 127.0.0.1:50051]
Needs only python3-grpcio and grpcio-tools (stubs are generated from proto/ at startup).
"""
import argparse
import json
import math
import pathlib
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import grpc
from grpc_tools import protoc

HERE = pathlib.Path(__file__).resolve().parent
PROTO_DIR = HERE.parent.parent / "proto"
GEN_DIR = HERE / "generated"

# Which limiter rule protects which route. Rules live in config/rules.yaml.
ROUTES = {
    "/api/hello": "demo",        # 5 burst, 1/s: easy to see 429 by hand
    "/api/data": "api_default",  # 1000 burst, 500/s
}
LIMITER_TIMEOUT_S = 0.2  # explicit deadline on every call to the limiter


def load_stubs():
    GEN_DIR.mkdir(exist_ok=True)
    rc = protoc.main([
        "grpc_tools.protoc", f"-I{PROTO_DIR}", f"--python_out={GEN_DIR}", f"--grpc_python_out={GEN_DIR}",
        str(PROTO_DIR / "rate_limiter.proto"),
    ])
    if rc != 0:
        sys.exit(f"protoc failed on {PROTO_DIR / 'rate_limiter.proto'}")
    sys.path.insert(0, str(GEN_DIR))
    import rate_limiter_pb2
    import rate_limiter_pb2_grpc
    return rate_limiter_pb2, rate_limiter_pb2_grpc


pb, pb_grpc = load_stubs()


class Gateway(BaseHTTPRequestHandler):
    limiter = None  # RateLimiterStub, shared by all handler threads (gRPC channels are thread-safe)

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        rule = ROUTES.get(path)
        if rule is None:
            return self.reply(404, {"error": "no such route", "routes": list(ROUTES)})

        # Who is being limited: the X-User-Id header, else the caller's IP.
        user = self.headers.get("X-User-Id")
        key = f"user:{user}" if user else f"ip:{self.client_address[0]}"

        try:
            resp = self.limiter.Check(pb.CheckRequest(key=key, rule=rule, cost=1), timeout=LIMITER_TIMEOUT_S)
        except grpc.RpcError as e:
            return self.reply(503, {"error": "rate limiter unavailable", "detail": f"{e.code().name}: {e.details()}"})

        headers = {"X-RateLimit-Limit": resp.limit, "X-RateLimit-Remaining": resp.remaining}
        if not resp.allowed:
            headers["Retry-After"] = max(1, math.ceil(resp.retry_after_ms / 1000))
            return self.reply(429, {"error": "too many requests", "retry_after_ms": resp.retry_after_ms}, headers)

        # Stand-in for forwarding to the real upstream service.
        return self.reply(200, {"message": f"hello {key}", "route": path, "rule": rule}, headers)

    def reply(self, status, body, headers=None):
        data = json.dumps(body).encode() + b"\n"
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        for k, v in (headers or {}).items():
            self.send_header(k, str(v))
        self.end_headers()
        self.wfile.write(data)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--listen", default="0.0.0.0:8080")
    ap.add_argument("--limiter", default="127.0.0.1:50051")
    args = ap.parse_args()

    channel = grpc.insecure_channel(args.limiter)  # one channel for the whole process
    Gateway.limiter = pb_grpc.RateLimiterStub(channel)

    host, port = args.listen.rsplit(":", 1)
    server = ThreadingHTTPServer((host, int(port)), Gateway)
    print(f"gateway on http://{args.listen} -> limiter {args.limiter}; routes: {ROUTES}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        channel.close()


if __name__ == "__main__":
    main()
