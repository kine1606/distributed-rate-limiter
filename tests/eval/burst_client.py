#!/usr/bin/env python3
"""Evaluation test 4 client (docs/EVALUATION.md): drives one hot key through several limiter instances
and records every call, so that the over-admission can be measured and the same demand replayed
through the model (tests/eval/bound_model.py).

    tests/eval/burst_client.py <targets,comma,separated> <rule> <key> <warm 0|1> <idle_s> <seconds> \\
        <calls per second per instance> <out.json>

warm 1: one check per instance first, which fills each instance's lease; then idle_s of silence,
while the bucket refills; then every instance at the given rate, open loop, for `seconds`. Writes
{"calls": [[instance, t_send, t_recv, allowed], ...], "errors": n, "send_lag_p99_s": x}; times are
seconds on one monotonic clock.
"""
import asyncio
import json
import sys
import time

import grpc

from quota_client import pb, pb_grpc


async def run(targets, rule, key, warm, idle_s, seconds, rate):
    channels = [grpc.aio.insecure_channel(t) for t in targets]
    stubs = [pb_grpc.RateLimiterStub(c) for c in channels]
    for c in channels:
        await c.channel_ready()
    calls, lags, errors = [], [], 0
    request = pb.CheckRequest(key=key, rule=rule, cost=1)

    async def one(i):
        nonlocal errors
        sent = time.monotonic()
        try:
            r = await stubs[i].Check(request, timeout=5)
            calls.append([i, sent, time.monotonic(), r.allowed])
        except grpc.aio.AioRpcError:
            errors += 1

    if warm:
        for i in range(len(stubs)):
            await one(i)
    await asyncio.sleep(idle_s)

    async def stream(i, t0, n):
        tasks = []
        for k in range(n):
            due = t0 + k / rate
            if due > time.monotonic():
                await asyncio.sleep(due - time.monotonic())
            lags.append(max(0.0, time.monotonic() - due))
            tasks.append(asyncio.ensure_future(one(i)))
        await asyncio.gather(*tasks)

    t0 = time.monotonic() + 0.05
    await asyncio.gather(*(stream(i, t0 + i * 1e-4, int(seconds * rate)) for i in range(len(stubs))))
    for c in channels:
        await c.close()
    lags.sort()
    return {"calls": calls, "errors": errors, "send_lag_p99_s": lags[int(0.99 * (len(lags) - 1))] if lags else 0}


def main():
    targets, rule, key = sys.argv[1].split(","), sys.argv[2], sys.argv[3]
    warm, idle_s, seconds, rate = sys.argv[4] == "1", float(sys.argv[5]), float(sys.argv[6]), float(sys.argv[7])
    out = asyncio.run(run(targets, rule, key, warm, idle_s, seconds, rate))
    with open(sys.argv[8], "w") as f:
        json.dump(out, f)


if __name__ == "__main__":
    main()
