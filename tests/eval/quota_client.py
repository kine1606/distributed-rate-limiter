#!/usr/bin/env python3
"""Evaluation test 1 client (docs/EVALUATION.md): hammers customers' quotas through several limiter
instances, with request ids, and resends a share of them to another instance at the same moment.

    tests/eval/quota_client.py <targets,comma,separated> <customer>=<calls> [...]
        env: RETRY=0.1  CONCURRENCY=64  RUN=1

Each call has a fresh request id; a RETRY share of them is sent twice at once, to two different
instances, which is the hardest case for deduplication (both reach Redis together). Prints one JSON
line: per customer, the ids sent, attempts, the ids that got at least one allowed answer, answers
by reason, ids whose two answers differed, and RPC errors.
"""
import asyncio
import json
import os
import random
import sys
import tempfile
from collections import Counter
from pathlib import Path

import grpc
from grpc_tools import protoc

PROTO_DIR = Path(__file__).resolve().parents[2] / "proto"


def load_stubs():
    out = tempfile.mkdtemp(prefix="rl_eval_stubs_")
    if protoc.main(["grpc_tools.protoc", f"-I{PROTO_DIR}", f"--python_out={out}", f"--grpc_python_out={out}",
                    str(PROTO_DIR / "rate_limiter.proto")]) != 0:
        sys.exit("protoc failed")
    sys.path.insert(0, out)
    import rate_limiter_pb2
    import rate_limiter_pb2_grpc
    return rate_limiter_pb2, rate_limiter_pb2_grpc


pb, pb_grpc = load_stubs()


async def run(targets, demand, retry, concurrency, run_id):
    channels = [grpc.aio.insecure_channel(t) for t in targets]
    stubs = [pb_grpc.RateLimiterStub(c) for c in channels]
    reason_name = {v: k.removeprefix("REASON_").lower() for k, v in pb.Reason.items()}
    stats = {c: {"ids": n, "attempts": 0, "allowed_ids": 0, "answers": Counter(), "mixed_ids": 0, "errors": 0}
             for c, n in demand.items()}
    gate = asyncio.Semaphore(concurrency)

    async def call(i, customer, rid):
        try:
            r = await stubs[i].Check(pb.CheckRequest(key=customer, rule="customer", cost=1, request_id=rid), timeout=10)
            return r
        except grpc.aio.AioRpcError as e:
            return e

    async def one(customer, rid):
        s = stats[customer]
        async with gate:
            i = random.randrange(len(stubs))
            if random.random() < retry:  # the same request at once on another instance
                j = (i + random.randrange(1, len(stubs))) % len(stubs)
                answers = await asyncio.gather(call(i, customer, rid), call(j, customer, rid))
            else:
                answers = [await call(i, customer, rid)]
        s["attempts"] += len(answers)
        allowed = []
        for a in answers:
            if isinstance(a, grpc.aio.AioRpcError):
                s["errors"] += 1
                continue
            s["answers"][reason_name[a.reason]] += 1
            allowed.append(a.allowed)
        if any(allowed):
            s["allowed_ids"] += 1
        if len(set(allowed)) > 1:
            s["mixed_ids"] += 1

    calls = [(c, f"{c}:{run_id}:{n}") for c, total in demand.items() for n in range(total)]
    random.shuffle(calls)
    await asyncio.gather(*(one(c, rid) for c, rid in calls))
    for ch in channels:
        await ch.close()
    return {c: {**s, "answers": dict(s["answers"])} for c, s in stats.items()}


def main():
    targets = sys.argv[1].split(",")
    demand = {k: int(v) for k, v in (a.split("=") for a in sys.argv[2:])}
    out = asyncio.run(run(targets, demand, float(os.environ.get("RETRY", "0.1")),
                          int(os.environ.get("CONCURRENCY", "64")), os.environ.get("RUN", "1")))
    print(json.dumps(out))


if __name__ == "__main__":
    main()
