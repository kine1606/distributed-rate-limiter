#!/usr/bin/env python3
"""Self time by category from perf stacks folded by FlameGraph's stackcollapse-perf.pl (bench/profile.sh):
each sample is charged to its leaf function's category, and "inside" columns give the share of samples
with such a frame anywhere on the stack.

    tools/profile_summary.py <name>.folded
    tools/profile_summary.py --self-check
"""
import re
import sys

# first match wins, on the leaf frame
CATEGORIES = [
    ("allocation", r"\bmalloc|\bfree\b|cfree|_int_free|_int_malloc|operator new|operator delete|gpr_malloc|gpr_free|"
                   r"tcache|malloc_consolidate|realloc|calloc|unlink_chunk|arena_"),
    ("locks and atomics", r"[Mm]utex|pthread_mutex|pthread_cond|futex|gpr_mu_|spin|SpinLock|lll_lock|CondVar"),
    ("memcpy and strings", r"memcpy|memmove|memset|memcmp|strlen|__strcmp|basic_string"),
    ("protobuf", r"google::protobuf|::_InternalSerialize|::_InternalParse|ByteSizeLong|MergeFrom|Clear\("),
    ("gRPC", r"grpc|absl::|gpr_|tsi_|census"),
    ("Redis client", r"sw::redis|redisAsync|redis[A-Z]|hiredis|sds|uv_|ev_"),
    ("rate limiter", r"ratelimiter::"),
]


def summarise(lines):
    total, self_by, inside = 0, {}, {"rate limiter": 0, "protobuf": 0, "allocation": 0}
    for line in lines:
        stack, _, count = line.rstrip().rpartition(" ")
        if not stack:
            continue
        n = int(count)
        frames = stack.split(";")
        total += n
        leaf = frames[-1]
        cat = next((c for c, rx in CATEGORIES if re.search(rx, leaf)), "other")
        self_by[cat] = self_by.get(cat, 0) + n
        for c in inside:
            rx = dict(CATEGORIES)[c]
            if any(re.search(rx, f) for f in frames):
                inside[c] += n
    return total, self_by, inside


def report(total, self_by, inside):
    out = [f"samples: {total}", "| Category (leaf frame) | Self time |", "| --- | --- |"]
    for cat, n in sorted(self_by.items(), key=lambda kv: -kv[1]):
        out.append(f"| {cat} | {100 * n / total:.1f}% |")
    out.append("")
    out += [f"inside {c}: {100 * n / total:.1f}% of samples" for c, n in inside.items()]
    return "\n".join(out) + "\n"


def self_check():
    folded = ["main;grpc::Server::Run;malloc 30", "main;ratelimiter::LeasedCore::Check;ratelimiter::TryTake 50",
              "main;ratelimiter::LeasedCore::Check;operator new 20"]
    total, self_by, inside = summarise(folded)
    assert total == 100 and self_by == {"allocation": 50, "rate limiter": 50}, self_by
    assert inside == {"rate limiter": 70, "protobuf": 0, "allocation": 50}, inside
    assert "| allocation | 50.0% |" in report(total, self_by, inside)
    print("profile_summary self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        with open(sys.argv[1]) as f:
            sys.stdout.write(report(*summarise(f)))
