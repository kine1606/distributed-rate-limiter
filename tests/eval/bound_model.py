#!/usr/bin/env python3
"""Evaluation test 12, the bound model (docs/EVALUATION.md): a simulation of N limiter instances
leasing one token bucket from Redis, written from the protocol of Chapter 3, 3.5.2, independently of
the C++ engine. It replays a demand (check arrival times per instance) and predicts which checks
are admitted; over_admission() then measures, on predicted or measured admissions alike, the largest
excess over what any exact bucket could admit in any window: max over a <= b of
admitted(a, b) - limit - rate * (b - a). The theorem of 3.5.3 bounds it by N x C.

    tests/eval/bound_model.py --self-check
    (as a module) simulate(arrivals, instances, limit, rate, chunk_cap, rtt_s) -> [(t, instance)]
"""
import heapq
import math
import random
import sys

CHUNK_IDLE_S = 10.0  # kChunkIdleNs


class Bucket:
    """lua/token_bucket.lua: milli-tokens, the Redis clock in whole milliseconds, a missing key is full."""

    def __init__(self, limit, rate):
        self.cap_m, self.rate = limit * 1000, rate
        self.tokens_m, self.last_ms = None, None

    def take(self, t, amount):  # -> (allowed, remaining, retry_after_ms)
        now = math.floor(t * 1000)
        if self.tokens_m is None:
            self.tokens_m, self.last_ms = self.cap_m, now
        self.tokens_m = min(self.cap_m, self.tokens_m + max(0, now - self.last_ms) * self.rate)
        self.last_ms = now
        need = amount * 1000
        if need <= self.tokens_m:
            self.tokens_m -= need
            return True, self.tokens_m // 1000, 0
        retry = math.ceil((need - self.tokens_m) / self.rate) if need <= self.cap_m and self.rate > 0 else 0
        return False, self.tokens_m // 1000, retry


class Lease:
    def __init__(self, cap):
        self.tokens, self.chunk, self.in_flight = 0, cap, False
        self.waiters, self.deny_until, self.last_refill = [], 0.0, None


def next_chunk(chunk, emptied, idle, cap):  # src/lease/chunk_policy.h
    nxt = chunk * 2 if emptied and not idle else chunk // 2 if idle and not emptied else chunk
    return max(min(8, cap), min(cap, nxt))


def simulate(arrivals, instances, limit, rate, chunk_cap, rtt_s=0.001):
    """arrivals: [(t_seconds, instance)], cost 1 each. Returns the admissions [(t, instance)]."""
    bucket, leases = Bucket(limit, rate), [Lease(chunk_cap) for _ in range(instances)]
    events, seq, admitted = [], 0, []

    def push(t, kind, *data):
        nonlocal seq
        heapq.heappush(events, (t, seq, kind, data))
        seq += 1

    def send(t, i, amount, partial_retry):  # the script runs at Redis half a round trip later
        push(t + rtt_s / 2, "redis", i, amount, partial_retry)

    def start_refill(t, i, target):
        lease = leases[i]
        if lease.in_flight:
            return
        lease.in_flight = True
        send(t, i, max(1, target - max(0, lease.tokens)), 0)

    def on_reply(t, i, requested, partial_retry, allowed, remaining, retry_after):
        lease = leases[i]
        if not allowed and 0 < remaining < requested:  # take what is left
            send(t, i, remaining, retry_after)
            return
        redis_out = not allowed or partial_retry > 0
        retry_ms = partial_retry if allowed else retry_after
        if allowed:
            lease.tokens += requested
            if partial_retry == 0:
                idle = lease.last_refill is not None and t - lease.last_refill > CHUNK_IDLE_S
                lease.chunk = next_chunk(lease.chunk, bool(lease.waiters), idle, chunk_cap)
                lease.last_refill = t
        if redis_out and retry_ms > 0:
            lease.deny_until = t + retry_ms / 1000
        lease.in_flight = False
        waiters, lease.waiters = lease.waiters, []
        for w in waiters:
            if lease.tokens >= 1:
                lease.tokens -= 1
                admitted.append((t, i))
            elif not redis_out:
                lease.waiters.append(w)  # a full chunk came and went: ask again
        if lease.waiters:
            start_refill(t, i, lease.chunk)

    for t, i in arrivals:
        push(t, "arrive", i)
    while events:
        t, _, kind, data = heapq.heappop(events)
        if kind == "arrive":
            i = data[0]
            lease = leases[i]
            if lease.tokens >= 1:
                lease.tokens -= 1
                admitted.append((t, i))
                if lease.tokens < lease.chunk * 3 // 10 and t >= lease.deny_until:
                    start_refill(t, i, lease.chunk)
            elif lease.deny_until <= t:
                lease.waiters.append(t)
                start_refill(t, i, lease.chunk)
        elif kind == "redis":
            i, amount, partial_retry = data
            push(t + rtt_s / 2, "reply", i, amount, partial_retry, *bucket.take(t, amount))
        else:
            on_reply(t, *data)
    return sorted(admitted)


def exact(arrivals, limit, rate):
    """What one exact bucket admits for the same arrivals: the strict path, one script per check."""
    bucket = Bucket(limit, rate)
    return [t for t, _ in sorted(arrivals) if bucket.take(t, 1)[0]]


def over_admission(times, limit, rate):
    """max over windows [t_i, t_j] of (j - i + 1) - limit - rate * (t_j - t_i); -limit if nothing.
    Redis refills by whole milliseconds, so even an exact bucket can show up to rate x 1 ms here."""
    best, out = -math.inf, -float(limit)
    for j, t in enumerate(sorted(times)):
        best = max(best, rate * t - j)  # i = j as a start, 0-based
        out = max(out, j + 1 + best - limit - rate * t)
    return out


def idle_burst_demand(instances, idle_s, burst_s, per_instance_rate, start=0.0):
    """One check per instance (fills its lease), idle while the bucket refills, then all at once."""
    warm = [(start + 0.01 * i, i) for i in range(instances)]
    t0 = start + 0.01 * instances + idle_s
    n = int(burst_s * per_instance_rate)
    return warm + [(t0 + k / per_instance_rate + i * 1e-5, i) for i in range(instances) for k in range(n)]


def self_check():
    # The worst case the proof allows: full bucket and full leases, then a burst. Each instance holds
    # C - 1 (its warm-up check took one token), so the excess approaches N x (C - 1): a little less,
    # because some refill lands while the window is open. Never above N x C.
    for cap in (10, 50, 100):
        demand = idle_burst_demand(4, idle_s=12, burst_s=2, per_instance_rate=500)
        over = over_admission([t for t, _ in simulate(demand, 4, 1000, 100, cap)], 1000, 100)
        assert 0.9 * 4 * (cap - 1) <= over <= 4 * cap, (cap, over)
    # Random demand never breaks the theorem, and an exact bucket never exceeds its own limit.
    rng = random.Random(7)
    for seed in range(200):
        n, limit, rate = rng.randint(1, 4), rng.randint(10, 2000), rng.randint(1, 500)
        cap = max(1, limit * rng.choice((1, 5, 10)) // 100)
        demand = sorted((rng.uniform(0, 5), rng.randrange(n)) for _ in range(rng.randint(10, 3000)))
        over = over_admission([t for t, _ in simulate(demand, n, limit, rate, cap)], limit, rate)
        tick = rate * 0.001  # Redis refills by whole milliseconds; the windows here are continuous
        assert over <= n * cap + tick, (seed, over, n * cap)
        assert over_admission(exact(demand, limit, rate), limit, rate) <= tick, seed
    print("bound_model self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        sys.exit(__doc__)
