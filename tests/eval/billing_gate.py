#!/usr/bin/env python3
"""The P3 billing gate (docs/PRODUCT_PLAN.md): a month's postpaid traffic through two limiters on the
fault cluster (3 masters with replicas, AOF), with gateway retries, and a master killed under load.
Then, per customer, what the gateway was told against what the quota counters kept, the invoice the
billing code makes from them, and the limiters' usage events. Every difference is reported, none is
corrected.

    control/.venv/bin/python tests/eval/billing_gate.py        env: SECONDS_RUN=20 KILL_AT=8 (0: no kill)
                                                                     CUSTOMERS=40 RATE=1500 (calls/s)
Needs build/release/rate_limiter_server and the fault cluster, which it starts if it is down
(docker compose -f docker/docker-compose.faults.yml). FLUSHALL on its masters first: test data only.
Output: results/<date>_billing_gate/ (summary.json, customers.tsv, the limiters' logs and usage files).

The month cannot end during a test, so the invoice is this month's, made by billing.preview, the code
that closes a month (billing.build), from the same counters. A gateway retries a call with the same
request id when the RPC fails or the answer is UNAVAILABLE, up to 6 times, as the proto asks.
"""
import asyncio
import datetime as dt
import json
import os
import pathlib
import random
import socket
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "control"))

import grpc  # noqa: E402
from grpc_tools import protoc  # noqa: E402
from redis.cluster import RedisCluster  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from rlcontrol import billing, periods, store  # noqa: E402
from rlcontrol.api import create_app  # noqa: E402

SEED_PORT = 7101
ADMIN = "billing-gate-operator-token"
PLAN = {"daily": 150, "burst": 1000, "refill": 1000, "overage": "postpaid", "allowance": 1_000_000,
        "failure": "closed", "fee": 1900, "overage_price": 50}


def stubs():
    out = tempfile.mkdtemp(prefix="rl_gate_stubs_")
    assert protoc.main(["protoc", f"-I{ROOT / 'proto'}", f"--python_out={out}", f"--grpc_python_out={out}",
                        str(ROOT / "proto/rate_limiter.proto")]) == 0
    sys.path.insert(0, out)
    import rate_limiter_pb2
    import rate_limiter_pb2_grpc
    return rate_limiter_pb2, rate_limiter_pb2_grpc


def sh(*cmd):
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def cluster_ok():
    try:
        return "cluster_state:ok" in sh("redis-cli", "-p", str(SEED_PORT), "cluster", "info")
    except subprocess.CalledProcessError:
        return False


def wait(predicate, seconds, what):
    end = time.time() + seconds
    while time.time() < end:
        if predicate():
            return
        time.sleep(0.2)
    sys.exit(f"timed out waiting for {what}")


def masters():
    """{port: [slot ranges]} of the masters that are up."""
    out = {}
    for line in sh("redis-cli", "-p", str(SEED_PORT), "cluster", "nodes").splitlines():
        f = line.split()
        if "master" in f[2] and "fail" not in f[2]:
            out[int(f[1].split("@")[0].split(":")[1])] = [tuple(map(int, (s.split("-") * 2)[:2])) for s in f[8:]
                                                          if not s.startswith("[")]
    return out


def owner(slot):
    return next(p for p, ranges in masters().items() if any(a <= slot <= b for a, b in ranges))


def listening(port):
    with socket.socket() as s:
        return s.connect_ex(("127.0.0.1", port)) == 0


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def load(pb, pb_grpc, targets, customers, seconds, rate, kill):
    """Calls at `rate` a second, spread over customers and limiters; each one retried with its request
    id until it gets a decision. Returns {customer: Counter of final reasons} and the log of the run."""
    channels = [grpc.aio.insecure_channel(t) for t in targets]
    rpc = [pb_grpc.RateLimiterStub(c) for c in channels]
    names = {v: k.removeprefix("REASON_").lower() for k, v in pb.Reason.items()}
    final = defaultdict(Counter)
    log = Counter()

    async def call(customer, rid):
        for attempt in range(6):
            stub = random.choice(rpc)
            try:
                r = await stub.Check(pb.CheckRequest(key=customer, rule="customer", cost=1, request_id=rid),
                                     metadata=[("x-api-key", targets.key)], timeout=1.0)
                reason = names[r.reason]
                if reason != "unavailable":
                    final[customer][reason] += 1
                    log["retried_ids"] += attempt > 0
                    return
                log["unavailable_answers"] += 1
            except grpc.aio.AioRpcError as e:
                log["rpc_" + e.code().name.lower()] += 1
            await asyncio.sleep(0.2 * (attempt + 1))
        final[customer]["gave_up"] += 1

    tasks, start, n = [], time.monotonic(), 0
    while (elapsed := time.monotonic() - start) < seconds:
        if kill and not log["killed"] and elapsed >= kill[0]:
            kill[1]()
            log["killed"] = 1
            log["killed_at_s"] = round(elapsed, 2)
        due = int(elapsed * rate)
        for _ in range(due - n):
            c = random.choice(customers)
            tasks.append(asyncio.ensure_future(call(c, f"{c}:{len(tasks)}")))
        n = due
        await asyncio.sleep(0.005)
    await asyncio.gather(*tasks)
    log["calls"] = len(tasks)
    for ch in channels:
        await ch.close()
    return final, log


class Targets(list):
    key = ""


def main():
    seconds = int(os.environ.get("SECONDS_RUN", "20"))
    kill_at = float(os.environ.get("KILL_AT", "8"))
    n_customers = int(os.environ.get("CUSTOMERS", "40"))
    rate = int(os.environ.get("RATE", "1500"))
    out = ROOT / "results" / f"{dt.date.today()}_billing_gate"
    out.mkdir(parents=True, exist_ok=True)
    server = ROOT / "build/release/rate_limiter_server"
    if not server.exists():
        sys.exit("build/release/rate_limiter_server is not built")

    if not cluster_ok():
        subprocess.run(["docker", "compose", "-p", "ratelimiter-faults", "-f", str(ROOT / "docker/docker-compose.faults.yml"),
                        "up", "-d"], check=True, capture_output=True)
        wait(cluster_ok, 60, "the fault cluster")
    for port in masters():
        sh("redis-cli", "-p", str(port), "FLUSHALL")
    r = RedisCluster(host="127.0.0.1", port=SEED_PORT, decode_responses=True)

    api = TestClient(create_app(redis=r, admin_token=ADMIN))
    merchant = "gate" + str(random.randrange(10 ** 6))
    body = api.post("/v1/merchants", json={"id": merchant}, headers={"Authorization": f"Bearer {ADMIN}"}).json()
    auth = {"Authorization": f"Bearer {body['management_key']}"}
    assert api.put("/v1/plans/pay", json=PLAN, headers=auth).status_code == 200
    customers = [f"u{i}" for i in range(n_customers)]
    for c in customers:
        assert api.post("/v1/customers", json={"id": c, "plan": "pay"}, headers=auth).status_code == 201

    pb, pb_grpc = stubs()
    targets, procs = Targets(), []
    targets.key = body["check_key"]
    for i in range(2):
        port = free_port()
        env = {**os.environ, "RL_TENANCY": "merchants", "RL_METRICS_ADDR": "off",
               "RL_USAGE_FILE": str(out / f"usage_{i}.jsonl")}
        (out / f"usage_{i}.jsonl").unlink(missing_ok=True)
        procs.append(subprocess.Popen([str(server), f"127.0.0.1:{port}", str(ROOT / "config/rules.yaml"),
                                       f"127.0.0.1:{SEED_PORT}", str(ROOT / "lua"), "leased"], env=env,
                                      stdout=open(out / f"limiter_{i}.log", "w"), stderr=subprocess.STDOUT))
        targets.append(f"127.0.0.1:{port}")
        wait(lambda p=port: listening(p), 10, "a limiter")
    time.sleep(1)  # the merchant's key loaded

    victim = {}

    def kill_master():  # the master holding the first customer's counter
        port = owner(r.keyslot(store.quota_key(merchant, customers[0])))
        victim["port"] = port
        victim["container"] = f"ratelimiter-faults-redis-{port - 7100}-1"
        sh("docker", "kill", victim["container"])

    final, log = asyncio.run(load(pb, pb_grpc, targets, [store.customer_id(merchant, c) for c in customers], seconds,
                                  rate, (kill_at, kill_master) if kill_at > 0 else None))
    for p in procs:
        p.terminate()  # flushes the usage events
    for p in procs:
        p.wait(timeout=15)
    if victim:
        wait(cluster_ok, 30, "the cluster after the kill")
        sh("docker", "start", victim["container"])  # rejoins as a replica

    events = defaultdict(Counter)
    for i in range(2):
        for line in (out / f"usage_{i}.jsonl").read_text().splitlines():
            if line.strip():
                e = json.loads(line)
                for f in ("allowed", "overage", "unverified", "unavailable"):
                    events[e["customer"]][f] += e.get(f, 0)

    rows, totals = [], Counter()
    for c in customers:
        cid = store.customer_id(merchant, c)
        told = final[cid]
        in_plan, billed = billing.month_totals(r, cid, periods.month_of(store.now_ms(r)))
        invoice = billing.preview(r, merchant, c)
        inv_units = sum(line["units"] for line in invoice["lines"] if line["item"] == "overage")
        row = {"customer": c, "told_ok": told["ok"], "told_overage": told["overage"], "gave_up": told["gave_up"],
               "counter_in_plan": in_plan, "counter_overage": billed, "invoice_overage": inv_units,
               "invoice_total_cents": invoice["total"], "events_allowed": events[cid]["allowed"],
               "events_overage": events[cid]["overage"]}
        rows.append(row)
        for k, v in row.items():
            if k != "customer":
                totals[k] += v or 0
        totals["customers_where_counter_differs"] += (in_plan, billed) != (told["ok"], told["overage"])
        totals["customers_where_invoice_differs_from_counter"] += inv_units != billed

    summary = {"merchant": merchant, "seconds": seconds, "rate": rate, "customers": n_customers,
               "plan": PLAN, "kill": victim or None, "log": dict(log), "totals": dict(totals),
               "differences": [row for row in rows if (row["counter_in_plan"], row["counter_overage"]) !=
                               (row["told_ok"], row["told_overage"]) or row["invoice_overage"] != row["counter_overage"]
                               or (row["events_allowed"], row["events_overage"]) != (row["told_ok"], row["told_overage"])]}
    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    with open(out / "customers.tsv", "w") as f:
        f.write("\t".join(rows[0]) + "\n")
        for row in rows:
            f.write("\t".join(str(v) for v in row.values()) + "\n")
    print(json.dumps({k: summary[k] for k in ("kill", "log", "totals")}, indent=2))
    print(f"{len(summary['differences'])} customers with a difference; details in {out}/summary.json")


if __name__ == "__main__":
    main()
