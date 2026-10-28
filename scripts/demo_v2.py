#!/usr/bin/env python3
"""Demo v2 (docs/PRODUCT_PLAN.md, P4): one merchant, its Free and Pay plans, and the two moments the
service is for, with a real limiter (RL_TENANCY=merchants) and the management API on the dev cluster.

  1. A paying customer in an emergency goes past its quota: postpaid admits it as billed overage, and
     the invoice preview shows the charge.
  2. A flood of Free calls: the merchant's capacity sheds them, while a paying customer keeps every call
     from the reserve.

Then the merchant portal is at the address printed, for the key printed.
    control/.venv/bin/python scripts/demo_v2.py        env: KEEP=1 keeps everything running until Ctrl-C
Needs build/release (rate_limiter_server, loadgen) and the dev cluster (docker/docker-compose.dev.yml).
The merchant it makes stays on the dev cluster, which is throwaway.
"""
import json
import os
import pathlib
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "control"))

import grpc  # noqa: E402
import httpx  # noqa: E402
from grpc_tools import protoc  # noqa: E402

SERVER, LOADGEN = ROOT / "build/release/rate_limiter_server", ROOT / "build/release/bench/loadgen"


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def wait_listening(port, what):
    for _ in range(100):
        with socket.socket() as s:
            if s.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.1)
    sys.exit(f"{what} did not start")


def stubs():
    out = tempfile.mkdtemp(prefix="rl_demo_")
    assert protoc.main(["protoc", f"-I{ROOT / 'proto'}", f"--python_out={out}", f"--grpc_python_out={out}",
                        str(ROOT / "proto/rate_limiter.proto")]) == 0
    sys.path.insert(0, out)
    import rate_limiter_pb2
    import rate_limiter_pb2_grpc
    return rate_limiter_pb2, rate_limiter_pb2_grpc


def say(text=""):
    print(text, flush=True)


def main():
    for f in (SERVER, LOADGEN):
        if not f.exists():
            sys.exit(f"{f.relative_to(ROOT)} is not built: scripts/build.sh release")
    if subprocess.run(["redis-cli", "-p", "7001", "cluster", "info"], capture_output=True, text=True).stdout.find(
            "cluster_state:ok") < 0:
        sys.exit("dev Redis Cluster not running: docker compose -p ratelimiter-dev -f docker/docker-compose.dev.yml up -d")

    admin, api_port, grpc_port = secrets.token_hex(16), free_port(), free_port()
    procs = [
        subprocess.Popen([str(ROOT / "control/.venv/bin/uvicorn"), "rlcontrol.api:app", "--app-dir", str(ROOT / "control"),
                          "--host", "127.0.0.1", "--port", str(api_port)],
                         env={**os.environ, "RL_ADMIN_TOKEN": admin, "RL_REDIS": "127.0.0.1:7001"},
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
        subprocess.Popen([str(SERVER), f"127.0.0.1:{grpc_port}", str(ROOT / "config/rules.yaml"), "127.0.0.1:7001",
                          str(ROOT / "lua"), "leased"],
                         env={**os.environ, "RL_TENANCY": "merchants", "RL_METRICS_ADDR": "off"},
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL),
    ]
    try:
        wait_listening(api_port, "the management API")
        wait_listening(grpc_port, "the limiter")
        run(admin, api_port, grpc_port)
        if os.environ.get("KEEP") == "1":
            say("\nRunning until Ctrl-C.")
            signal.sigwait({signal.SIGINT, signal.SIGTERM})
    finally:
        for p in procs:
            p.terminate()
        for p in procs:
            p.wait(timeout=10)


def run(admin, api_port, grpc_port):
    api = httpx.Client(base_url=f"http://127.0.0.1:{api_port}", timeout=10)
    merchant = "acme-" + secrets.token_hex(3)
    created = api.post("/v1/merchants", json={"id": merchant}, headers={"Authorization": f"Bearer {admin}"}).json()
    auth = {"Authorization": f"Bearer {created['management_key']}"}
    check_key = created["check_key"]

    say(f"== Merchant {merchant} defines its plans")
    # Bursts are generous on purpose: in act 2 the merchant's capacity, not each customer's burst, refuses.
    plans = {"free": {"daily": 100_000, "burst": 1000, "refill": 1000},
             "pay": {"daily": 100, "burst": 1000, "refill": 1000, "overage": "postpaid", "allowance": 1000,
                     "fee": 1900, "overage_price": 50}}
    for name, terms in plans.items():
        api.put(f"/v1/plans/{name}", json=terms, headers=auth).raise_for_status()
        say(f"   {name}: {json.dumps(terms)}")
    customers = [(f"free-{i}", "free") for i in range(1, 21)] + [("vip-1", "pay"), ("paid-1", "pay")]
    for cid, plan in customers:
        api.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth).raise_for_status()
    say(f"   customers: free-1..free-20 on free; vip-1 and paid-1 on pay")

    pb, rpc = stubs()
    stub = rpc.RateLimiterStub(grpc.insecure_channel(f"127.0.0.1:{grpc_port}"))

    def check(customer):
        r = stub.Check(pb.CheckRequest(key=customer, rule="customer"), metadata=[("x-api-key", check_key)], timeout=2)
        return pb.Reason.Name(r.reason).removeprefix("REASON_")

    for _ in range(50):  # the limiter loads the new merchant's key on the plan-changes message
        try:
            check("free-1")
            break
        except grpc.RpcError:
            time.sleep(0.1)

    say("\n== 1. vip-1, on pay (100 calls a day, postpaid), needs 130 calls today")
    answers = [check("vip-1") for _ in range(130)]
    counts = {a: answers.count(a) for a in dict.fromkeys(answers)}
    say(f"   answers: {counts}  (no call refused: the 30 beyond the quota are billed overage)")
    preview = api.get("/v1/customers/vip-1/invoice-preview", headers=auth).json()
    for line in preview["lines"]:
        units = "" if line["item"] == "fee" else f"{line['units']} calls, "
        say(f"   invoice preview: {line['description']}: {units}${line['amount'] / 100:.2f}")
    say(f"   total so far this month: ${preview['total'] / 100:.2f}")

    say("\n== 2. The merchant's backend takes 100 calls a second, 30% kept for plans with a fee")
    api.put("/v1/capacity", json={"rate": 100, "reserve": 30}, headers=auth).raise_for_status()
    time.sleep(0.5)  # every limiter reloads it on the plan-changes message
    say("   Free customers flood at 800 calls a second; paid-1 sends its usual 10 a second, for 8 s")
    env = {**os.environ, "RL_API_KEYS": check_key}
    load = lambda prefix, keys, rate: subprocess.Popen(  # noqa: E731
        [str(LOADGEN), f"127.0.0.1:{grpc_port}", "customer", "8", "4", "512", str(keys), "0", "1", str(rate), prefix],
        env=env, stdout=subprocess.PIPE, text=True)
    flood, paid = load("free-", 20, 800), load("paid-", 1, 10)
    results = {name: json.loads(p.communicate()[0].strip().splitlines()[-1]) for name, p in (("free", flood), ("paid", paid))}
    for name, r in results.items():
        o = r["outcomes"]
        say(f"   {name}: sent {r['ok']} ({r['sent_per_s']:.0f} a second); admitted {r['allowed']} "
            f"({r['allowed'] / 8:.0f} a second); refused for capacity {o.get('rate_limited', 0)}")
    say("   Free gets about the 70 a second left outside the reserve; paid-1 loses nothing.")

    say(f"\n== The merchant portal: http://127.0.0.1:{api_port}/portal")
    say(f"   management key: {created['management_key']}")


if __name__ == "__main__":
    main()
