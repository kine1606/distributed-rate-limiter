#!/usr/bin/env python3
"""The P4 evaluation of merchant tenancy (docs/PRODUCT_PLAN.md, Week P4), on the real limiter
(RL_TENANCY=merchants, engine leased) and the THROWAWAY dev cluster, which it FLUSHALLs. Billing
exactness, the sixth measurement, is tests/eval/billing_gate.py.

  isolation  two merchants with a customer of the same name: one's calls, keys and management key
             never reach the other's (pass or fail, one line each)
  noisy      a merchant flooding at 20,000 calls a second, by kind of flood, against another
             merchant's 1,000 customers at 5,000 a second (as tests/eval/noisy_neighbour.sh)
  latency    from the management API's answer to the limiter enforcing it: suspend, resume, quota
             down and up, boost on and off, capacity on and off; under background load
  capacity   a merchant's capacity (1,000 calls a second, 30% reserved for plans with a fee) under a
             Free flood: what Free and paying customers get, on one limiter and on two
  scale      10,000 merchants of 100 customers: seeding, Redis memory, the limiter's memory and
             start, throughput, and how soon a new merchant's key works
  cost       the cost of tenancy: the customer rule over Zipf 10,000 Pay customers, as
             bench/ab_compare.sh, without tenancy, with it, and with a capacity and check rate too;
             alternating runs

    control/.venv/bin/python tests/eval/tenancy_eval.py [section ...]     env: RUNS=3 (cost: 5)
                                                                          SERVER_CPUS=0-5 CLIENT_CPUS=6-11
Needs build/release (rate_limiter_server, loadgen) and the dev cluster. Output: results/<date>_tenancy/,
one <section>.json each, and a summary on stdout.
"""
import json
import os
import pathlib
import re
import secrets
import socket
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(ROOT / "control"), str(ROOT / "scripts")]

import grpc  # noqa: E402
import redis  # noqa: E402
from demo_v2 import stubs, wait_listening  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402
from redis.cluster import RedisCluster  # noqa: E402
from rlcontrol import store  # noqa: E402
from rlcontrol.api import create_app  # noqa: E402

SERVER, LOADGEN = ROOT / "build/release/rate_limiter_server", ROOT / "build/release/bench/loadgen"
PORT, LPORT = 7001, 30081  # the dev cluster; the limiter below Linux's ephemeral range
SCPUS, CCPUS = os.environ.get("SERVER_CPUS", "0-5"), os.environ.get("CLIENT_CPUS", "6-11")
ADMIN = "tenancy-eval-operator-token"
OUT = ROOT / "results" / f"{time.strftime('%Y-%m-%d')}_tenancy"
# The plan catalogue of the other benchmarks (tools/plans.sh), plus the ones below.
PLANS = dict(re.findall(r'\[(\w+)\]="([^"]+)"', (ROOT / "tools/plans.sh").read_text()))
PLANS["roomy"] = "daily 100000000 burst 100000 refill 100000 overage hard extra 0 failure closed"  # only capacity refuses
PLANS["roomy_paid"] = PLANS["roomy"] + " fee 1900"
PLANS["free_wide"] = "daily 200 burst 100000 refill 100000 overage hard extra 0 failure closed"  # out of quota at once
PLANS["narrow"] = "daily 50000 burst 20 refill 20 overage hard extra 0 failure closed"  # over its burst at once

rc = RedisCluster(host="127.0.0.1", port=PORT, decode_responses=True)
pb, rpc = stubs()


# ---- Redis ----

def masters():
    return sorted(n.port for n in rc.get_primaries())


def flushall():
    for p in masters():
        node = redis.Redis(port=p)
        if node.config_get("save")["save"]:  # the throwaway instances run with save ""
            sys.exit(f"Redis on {p} saves its data: not a throwaway instance, not flushed")
        node.flushall()


def info_sum(section, field):
    return sum(int(redis.Redis(port=p).info(section)[field]) for p in masters())


def redis_calls():
    total = 0
    for p in masters():
        stats = redis.Redis(port=p).info("commandstats")
        total += sum(stats.get(f"cmdstat_{c}", {}).get("calls", 0) for c in ("evalsha", "eval", "hgetall"))
    return total


def seed(lines):
    """Writes inline commands with redis-cli --pipe, each to the master of its key's slot (the key is
    the command's second word). Exits on any error."""
    by_master, slots = {}, {}
    for line in lines:
        key = line.split(" ", 2)[1]
        slot = rc.keyslot(key)
        port = slots.get(slot) or slots.setdefault(slot, rc.nodes_manager.get_node_from_slot(slot).port)
        by_master.setdefault(port, []).append(line)
    for port, cmds in by_master.items():
        with tempfile.NamedTemporaryFile("w", suffix=".redis") as f:
            f.write("\n".join(cmds) + "\n")
            f.flush()
            with open(f.name) as data:
                done = subprocess.run(["redis-cli", "-p", str(port), "--pipe"], stdin=data, capture_output=True, text=True)
        if "errors: 0," not in done.stdout:
            sys.exit(f"seeding {port} failed: {done.stdout[-300:]} {done.stderr[-300:]}")


def merchant_lines(m, check_key, customers, capacity=None, check_rate=None):
    """A merchant as the API writes it (control/rlcontrol/api.py), customers as (id, plan) pairs."""
    yield f"HSET {store.KEYS_HASH} {store.sha256_hex(check_key)} {m}"
    yield f"HSET {store.MERCHANTS} {m} 0"
    for name in sorted({plan for _, plan in customers}):
        yield f'HSET {store.catalog_key(m)} {name} "{PLANS[name]}"'
    for cid, plan in customers:
        yield f"HSET {store.plan_key(m, cid)} plan {plan} {PLANS[plan]}"
    for i in range(0, len(customers), 500):
        yield f"ZADD {store.customers_key(m)} " + " ".join(f"0 {cid}" for cid, _ in customers[i:i + 500])
    if capacity:
        yield f'HSET {store.CAPACITY_HASH} {m} "{capacity}"'
    if check_rate:
        yield f'HSET {store.CHECK_RATE_HASH} {m} "{check_rate}"'


def new_key():
    return store.new_key("rl_")


# ---- the limiter and the load ----

class Limiter:
    def __init__(self, port=LPORT, tenancy=True, cpus=SCPUS):
        env = {**os.environ, "RL_METRICS_ADDR": "off", "RL_TENANCY": "merchants" if tenancy else "off"}
        self.started = time.perf_counter()
        self.proc = subprocess.Popen(["taskset", "-c", cpus, str(SERVER), f"127.0.0.1:{port}", str(ROOT / "config/rules.yaml"),
                                      f"127.0.0.1:{PORT}", str(ROOT / "lua"), "leased"],
                                     env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port
        wait_listening(port, "the limiter")
        self.stub = rpc.RateLimiterStub(grpc.insecure_channel(f"127.0.0.1:{port}"))

    def check(self, customer, key):
        try:
            r = self.stub.Check(pb.CheckRequest(key=customer, rule="customer"), metadata=[("x-api-key", key)], timeout=2)
            return pb.Reason.Name(r.reason).removeprefix("REASON_")
        except grpc.RpcError as e:
            return e.code().name

    def ready(self, customer, key, limit_s=30):  # the merchants' keys loaded: seconds from the start
        end = time.perf_counter() + limit_s
        while self.check(customer, key) == "UNAUTHENTICATED":
            if time.perf_counter() > end:
                sys.exit("the limiter never accepted the merchant's key")
            time.sleep(0.01)
        return time.perf_counter() - self.started

    def rss_mb(self):  # the server under taskset: taskset execs it, so the pid is the server's
        status = pathlib.Path(f"/proc/{self.proc.pid}/status").read_text()
        return int(re.search(r"VmRSS:\s+(\d+)", status).group(1)) / 1024

    def stop(self):
        self.proc.terminate()
        self.proc.wait(timeout=10)


def loadgen(port, seconds, channels, in_flight, keys, warmup, rate, prefix, dist="uniform", api_keys=None,
            background=False):
    """bench/loadgen on the customer rule: its JSON line (or the process, with background)."""
    env = dict(os.environ)
    if isinstance(api_keys, str):
        env["RL_API_KEYS_FILE"] = api_keys
    elif api_keys:
        env["RL_API_KEYS"] = ",".join(api_keys)
    cmd = ["taskset", "-c", CCPUS, str(LOADGEN), f"127.0.0.1:{port}", "customer", str(seconds), str(channels),
           str(in_flight), str(keys), str(warmup), "1", str(rate), prefix, dist]
    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, text=True)
    return p if background else result(p)


def result(p):
    out = p.communicate()[0].strip().splitlines()
    if not out:
        sys.exit("loadgen printed nothing")
    return json.loads(out[-1])


def brief(r):  # a loadgen result without its timeline
    return {k: v for k, v in r.items() if k not in ("timeline", "timeline_fields")}


def median(xs):
    return statistics.median(xs) if xs else None


def save(name, data):
    OUT.mkdir(parents=True, exist_ok=True)
    meta = {"commit": subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                                     text=True).stdout.strip(),
            "dirty": bool(subprocess.run(["git", "status", "--porcelain"], cwd=ROOT, capture_output=True,
                                         text=True).stdout.strip()),
            "server_cpus": SCPUS, "client_cpus": CCPUS, "at": time.strftime("%Y-%m-%d %H:%M:%S")}
    (OUT / f"{name}.json").write_text(json.dumps({"meta": meta, **data}, indent=1))


def say(text=""):
    print(text, flush=True)


# ---- the sections ----

def isolation():
    flushall()
    api = TestClient(create_app(redis=rc, admin_token=ADMIN))
    op = {"Authorization": f"Bearer {ADMIN}"}
    a, b = (api.post("/v1/merchants", json={"id": m}, headers=op).json() for m in ("shop-a", "shop-b"))
    auth = {m["id"]: {"Authorization": f"Bearer {m['management_key']}"} for m in (a, b)}
    for m in (a, b):
        api.put("/v1/plans/tiny", json={"daily": 5, "burst": 1000, "refill": 1000}, headers=auth[m["id"]])
        api.post("/v1/customers", json={"id": "cust-1", "plan": "tiny"}, headers=auth[m["id"]])
    api.post("/v1/customers", json={"id": "only-b", "plan": "tiny"}, headers=auth["shop-b"])
    lim = Limiter()
    lim.ready("cust-1", a["check_key"])
    checks = []

    def expect(name, got, want):
        checks.append({"check": name, "pass": got == want, "got": got, "want": want})

    try:
        expect("A's customer: 5 within its quota of 5 (one was the readiness probe), then refused",
               [lim.check("cust-1", a["check_key"]) for _ in range(5)], ["OK"] * 4 + ["QUOTA_EXHAUSTED"])
        expect("B's customer of the same name is untouched", lim.check("cust-1", b["check_key"]), "OK")
        used = lambda m: api.get("/v1/customers/cust-1", headers=auth[m]).json()["usage"]["used"]  # noqa: E731
        expect("each merchant's API sees only its own customer's usage", (used("shop-a"), used("shop-b")), (5, 1))
        expect("A's management key cannot see B's customer",
               api.get("/v1/customers/only-b", headers=auth["shop-a"]).status_code, 404)
        expect("A's management key cannot change B's customer",
               api.post("/v1/customers/only-b/suspend", headers=auth["shop-a"]).status_code, 404)
        for _ in range(20):  # A names B's customer in the key: still A's namespace
            lim.check("shop-b|cust-1", a["check_key"])
        expect("a key naming B's customer under A's API key is not B's customer", used("shop-b"), 1)
        expect("an unknown API key is refused", lim.check("cust-1", new_key()), "UNAUTHENTICATED")
        expect("A's check key does not manage A", api.get("/v1/plans", headers={
            "Authorization": f"Bearer {a['check_key']}"}).status_code, 401)
        api.delete("/v1/merchants/shop-a/check-keys", headers=op)
        time.sleep(0.5)
        expect("a revoked check key is refused at once", lim.check("cust-1", a["check_key"]), "UNAUTHENTICATED")
    finally:
        lim.stop()
    save("isolation", {"checks": checks})
    for c in checks:
        say(f"  {'pass' if c['pass'] else 'FAIL'}  {c['check']}" + ("" if c["pass"] else f" (got {c['got']})"))
    return all(c["pass"] for c in checks)


NOISY = ("none", "bad_key", "over_check_rate", "over_capacity", "over_quota", "over_burst")


def noisy(runs):
    rows = []
    for run in range(1, runs + 1):
        for scenario in NOISY:
            flushall()
            good, bad = new_key(), new_key()
            plan = {"over_quota": "free_wide", "over_burst": "narrow"}.get(scenario, "roomy")
            seed(list(merchant_lines("good", good, [(f"pay:{i}", "pay") for i in range(1, 1001)])) + list(merchant_lines(
                "noisy", bad, [("n:1", plan)],
                capacity="rate 100 burst 100" if scenario == "over_capacity" else None,
                check_rate="rate 100 burst 100" if scenario == "over_check_rate" else None)))
            lim = Limiter()
            lim.ready("pay:1", good)
            if scenario == "over_quota":
                for _ in range(200):
                    lim.check("n:1", bad)
            flood = None
            if scenario != "none":
                flood = loadgen(LPORT, 13, 4, 4000, 1, 0, 20000, "n:", api_keys=[new_key() if scenario == "bad_key" else bad],
                                background=True)
                time.sleep(1)
            c0, t0 = redis_calls(), time.perf_counter()
            others = loadgen(LPORT, 10, 4, 2000, 1000, 1, 5000, "pay:", api_keys=[good])
            calls_per_s = (redis_calls() - c0) / (time.perf_counter() - t0)
            noise = result(flood) if flood else None
            lim.stop()
            row = {"scenario": scenario, "run": run, "redis_calls_per_s": round(calls_per_s),
                   "others": brief(others), "noisy": brief(noise) if noise else None}
            rows.append(row)
            say(f"  run {run} {scenario:15} others p50 {others['p50_ms']:.2f} p99 {others['p99_ms']:.2f} ms, "
                f"ok {others['ok']}, allowed {others['allowed']}, errors {others['errors']}; Redis {row['redis_calls_per_s']}/s")
    save("noisy", {"rows": rows})
    say("\n  | Noisy merchant | Others' p50 ms | p99 ms | p999 ms | Others allowed / answered | Redis calls/s |")
    for s in NOISY:
        rs = [r for r in rows if r["scenario"] == s]
        say(f"  | {s} | {median([r['others']['p50_ms'] for r in rs]):.3f} | {median([r['others']['p99_ms'] for r in rs]):.3f} | "
            f"{median([r['others']['p999_ms'] for r in rs]):.3f} | {min(r['others']['allowed'] for r in rs)} / "
            f"{min(r['others']['ok'] for r in rs)} (worst run) | {median([r['redis_calls_per_s'] for r in rs]):.0f} |")


def latency(trials):
    flushall()
    load_key = new_key()
    seed(list(merchant_lines("load", load_key, [(f"pay:{i}", "pay") for i in range(1, 1001)])))
    api = TestClient(create_app(redis=rc, admin_token=ADMIN))
    m = api.post("/v1/merchants", json={"id": "lat"}, headers={"Authorization": f"Bearer {ADMIN}"}).json()
    auth, key = {"Authorization": f"Bearer {m['management_key']}"}, m["check_key"]
    roomy = {"burst": 100000, "refill": 100000}
    api.put("/v1/plans/big", json={"daily": 1_000_000, **roomy}, headers=auth)  # a leased quota (>= 100,000)
    api.put("/v1/plans/small", json={"daily": 3, **roomy}, headers=auth)        # the exact path
    for cid, plan in (("c-big", "big"), ("c-small", "small")):
        api.post("/v1/customers", json={"id": cid, "plan": plan}, headers=auth)
    lim = Limiter()
    lim.ready("c-big", key)
    for _ in range(5):
        lim.check("c-small", key)  # past its quota of 3: the deny cache holds it
    rate = int(os.environ.get("LOAD", "20000"))
    bg = loadgen(LPORT, 3600, 4, 2000, 1000, 0, rate, "pay:", api_keys=[load_key], background=True) if rate else None
    time.sleep(2)
    # (name, customer, request, the answer that shows it is in force)
    changes = [
        ("suspend", "c-big", lambda: api.post("/v1/customers/c-big/suspend", headers=auth), {"SUSPENDED"}),
        ("resume", "c-big", lambda: api.post("/v1/customers/c-big/resume", headers=auth), {"OK"}),
        ("quota down (leased quota)", "c-big", lambda: api.patch("/v1/customers/c-big/limits", json={"daily": 1}, headers=auth),
         {"QUOTA_EXHAUSTED"}),
        ("quota up", "c-big", lambda: api.patch("/v1/customers/c-big/limits", json={"daily": 1_000_000}, headers=auth), {"OK"}),
        ("boost on (denied customer)", "c-small",
         lambda: api.post("/v1/customers/c-small/boost", json={"units": 1_000_000, "minutes": 60}, headers=auth), {"OK"}),
        ("boost off", "c-small", lambda: api.delete("/v1/customers/c-small/boost", headers=auth), {"QUOTA_EXHAUSTED"}),
        ("capacity on (1 a second)", "c-big", lambda: api.put("/v1/capacity", json={"rate": 1}, headers=auth), {"RATE_LIMITED"}),
        ("capacity off", "c-big", lambda: api.delete("/v1/capacity", headers=auth), {"OK"}),
    ]
    rows = {name: [] for name, *_ in changes}
    try:
        for _ in range(trials):
            for name, cid, request, want in changes:
                t0 = time.perf_counter()
                request().raise_for_status()
                t1, stale = time.perf_counter(), 0
                while (answer := lim.check(cid, key)) not in want:
                    stale += 1
                    if time.perf_counter() - t1 > 10:
                        sys.exit(f"{name}: still {answer} after 10 s")
                t2 = time.perf_counter()
                rows[name].append({"api_ms": (t1 - t0) * 1e3, "after_api_ms": (t2 - t1) * 1e3, "stale_answers": stale})
    finally:
        if bg:
            bg.terminate()
            bg.wait()
        lim.stop()
    save("latency", {"trials": trials, "background_load_per_s": rate, "rows": rows})
    say(f"\n  {trials} trials each, background load {rate} calls a second")
    say("  | Change | API call ms (p50) | Enforced after the API answered, ms: p50 / p99 / max | Old answers after it (max) |")
    for name, rs in rows.items():
        after = sorted(r["after_api_ms"] for r in rs)
        say(f"  | {name} | {median([r['api_ms'] for r in rs]):.1f} | {median(after):.2f} / "
            f"{after[min(len(after) - 1, int(len(after) * 0.99))]:.2f} / {after[-1]:.2f} | {max(r['stale_answers'] for r in rs)} |")


def capacity(runs):
    shapes = [("1 limiter, paid 100/s", 1, 100), ("1 limiter, paid 300/s", 1, 300), ("1 limiter, paid 600/s", 1, 600),
              ("2 limiters, paid 300/s", 2, 300)]
    rows = []
    for run in range(1, runs + 1):
        for name, n, paid_rate in shapes:
            flushall()
            key = new_key()
            customers = [(f"free:{i}", "roomy") for i in range(1, 101)] + [(f"paid:{i}", "roomy_paid") for i in range(1, 11)]
            seed(list(merchant_lines("shop", key, customers, capacity="rate 1000 burst 1000 reserve 30")))
            lims = [Limiter(LPORT + i) for i in range(n)]
            lims[0].ready("free:1", key)
            procs = []
            for lim in lims:  # warm 2 s (the buckets start full), then 10 s measured
                procs.append(("free", loadgen(lim.port, 10, 4, 1000, 100, 2, 5000 // n, "free:", api_keys=[key], background=True)))
                procs.append(("paid", loadgen(lim.port, 10, 2, 500, 10, 2, paid_rate // n, "paid:", api_keys=[key], background=True)))
            got = {"free": [], "paid": []}
            for kind, p in procs:
                got[kind].append(brief(result(p)))
            for lim in lims:
                lim.stop()
            per_s = {k: sum(r["allowed"] for r in v) / 10 for k, v in got.items()}
            sent = {k: sum(r["ok"] for r in v) / 10 for k, v in got.items()}
            rows.append({"shape": name, "run": run, "limiters": n, "paid_rate": paid_rate, "admitted_per_s": per_s,
                         "answered_per_s": sent, "results": got})
            say(f"  run {run} {name}: free {per_s['free']:.0f}/s of {sent['free']:.0f}, paid {per_s['paid']:.0f}/s of "
                f"{sent['paid']:.0f}, total {per_s['free'] + per_s['paid']:.0f}/s")
    save("capacity", {"capacity": "rate 1000 burst 1000 reserve 30", "rows": rows})


def scale(merchants=10_000, per=100, runs=3):
    flushall()
    mem0 = info_sum("memory", "used_memory")
    keys = [new_key() for _ in range(merchants)]
    customers = [(f"cust-{i}", "pay" if i <= per // 5 else "free") for i in range(1, per + 1)]
    t = time.perf_counter()
    lines = []
    for i, k in enumerate(keys):
        lines += merchant_lines(f"m{i:05d}", k, customers)
        if len(lines) > 200_000:
            seed(lines)
            lines = []
    seed(lines)
    seed_s = time.perf_counter() - t
    mem1 = info_sum("memory", "used_memory")
    with tempfile.NamedTemporaryFile("w", suffix=".keys", delete=False) as f:
        f.write("\n".join(keys) + "\n")
    lim = Limiter()
    try:
        start_s = lim.ready("cust-1", keys[-1])
        tput = []
        for run in range(1, runs + 1):
            r = brief(loadgen(LPORT, 12, 12, 16, per, 3, 0, "cust-", api_keys=f.name))
            tput.append(r)
            say(f"  run {run}: {r['calls_per_s']:.0f} calls/s, p50 {r['p50_ms']:.3f} p99 {r['p99_ms']:.3f} ms, "
                f"allowed {r['allowed']} of {r['ok']}, errors {r['errors']}")
        rss = lim.rss_mb()
        mem2 = info_sum("memory", "used_memory")
        new_merchant = []
        for i in range(10):  # a new merchant's key, from the publish to its first accepted check
            k = new_key()
            seed(list(merchant_lines(f"new{i}", k, [("cust-1", "pay")])))
            t0 = time.perf_counter()
            rc.publish(store.CHANNEL, store.MERCHANTS_CHANGED)
            while lim.check("cust-1", k) == "UNAUTHENTICATED":
                if time.perf_counter() - t0 > 10:
                    sys.exit("a new merchant's key never worked")
            new_merchant.append((time.perf_counter() - t0) * 1e3)
    finally:
        lim.stop()
        os.unlink(f.name)
    keys_total = sum(redis.Redis(port=p).dbsize() for p in masters())
    data = {"merchants": merchants, "customers_each": per, "seed_s": seed_s, "redis_used_bytes": [mem0, mem1, mem2],
            "keys_after": keys_total, "limiter_start_s": start_s, "limiter_rss_mb": rss, "throughput": tput,
            "new_merchant_ms": new_merchant}
    save("scale", data)
    n = merchants * per
    say(f"\n  seeded {merchants:,} merchants x {per} customers in {seed_s:.0f} s; Redis +{(mem1 - mem0) / 2**20:.0f} MiB "
        f"({(mem1 - mem0) / n:.0f} B a customer, merchant keys and catalogue included)")
    say(f"  after the load: Redis {mem2 / 2**20:.0f} MiB, {keys_total:,} keys; limiter RSS {rss:.0f} MiB; "
        f"started and loaded the keys in {start_s:.2f} s")
    say(f"  throughput median {median([r['calls_per_s'] for r in tput]):.0f} calls/s; a new merchant's key works "
        f"{median(new_merchant):.1f} ms after the publish (max {max(new_merchant):.1f})")


def cost(runs):
    variants = ("plain", "tenancy", "tenancy+limits")
    rows = []
    for run in range(1, runs + 1):
        for v in variants:
            flushall()
            key = new_key()
            customers = [(f"cust:pay:{i}", "pay") for i in range(1, 10_001)]
            if v == "plain":
                seed([f"HSET {{{cid}}}:plan plan pay {PLANS['pay']}" for cid, _ in customers])
            else:
                big = "rate 4000000000 burst 4000000000" if v == "tenancy+limits" else None
                seed(list(merchant_lines("m1", key, customers, capacity=big, check_rate=big)))
            lim = Limiter(tenancy=v != "plain")
            if v != "plain":
                lim.ready("cust:pay:1", key)
            r = brief(loadgen(LPORT, 12, 12, 16, 10_000, 3, 0, "cust:pay:", "zipf", api_keys=[key] if v != "plain" else None))
            lim.stop()
            rows.append({"variant": v, "run": run, "result": r})
            say(f"  run {run} {v:15} {r['calls_per_s']:.0f} calls/s, p50 {r['p50_ms']:.3f} p99 {r['p99_ms']:.3f} ms, "
                f"allowed {r['allowed']} of {r['ok']}, errors {r['errors']}")
    save("cost", {"rows": rows})
    base = median([r["result"]["calls_per_s"] for r in rows if r["variant"] == "plain"])
    say("\n  | Variant | calls/s, median (range) | vs plain | p50 ms | p99 ms |")
    for v in variants:
        rs = [r["result"] for r in rows if r["variant"] == v]
        c = [r["calls_per_s"] for r in rs]
        say(f"  | {v} | {median(c):.0f} ({min(c):.0f} to {max(c):.0f}) | {100 * (median(c) / base - 1):+.1f}% | "
            f"{median([r['p50_ms'] for r in rs]):.3f} | {median([r['p99_ms'] for r in rs]):.3f} |")


def main():
    for f in (SERVER, LOADGEN):
        if not f.exists():
            sys.exit(f"{f.relative_to(ROOT)} is not built: scripts/build.sh release")
    sections = sys.argv[1:] or ["isolation", "noisy", "latency", "capacity", "scale", "cost"]
    runs = int(os.environ.get("RUNS", "3"))
    ok = True
    for s in sections:
        say(f"== {s}")
        if s == "isolation":
            ok = isolation() and ok
        elif s == "noisy":
            noisy(runs)
        elif s == "latency":
            latency(int(os.environ.get("TRIALS", "30")))
        elif s == "capacity":
            capacity(runs)
        elif s == "scale":
            scale(int(os.environ.get("MERCHANTS", "10000")), int(os.environ.get("PER", "100")), runs)
        elif s == "cost":
            cost(int(os.environ.get("RUNS", "5")))
        else:
            sys.exit(f"unknown section {s}")
    say(f"\nraw output: {OUT}")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
