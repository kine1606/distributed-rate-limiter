#!/usr/bin/env python3
"""Universal Scalability Law fit and Little's law check over bench/concurrency_sweep.sh (Week 8).

USL (Gunther): X(N) = lam * N / (1 + sigma * (N - 1) + kappa * N * (N - 1)), N calls in flight,
fitted to every run by least squares; sigma is contention (queueing on something shared), kappa is
crosstalk (cost that grows with pairs of calls). Peak at N* = sqrt((1 - sigma) / kappa).
Little's law: in a closed loop N = X * R, so N / X is the mean latency; it must lie between the
measured p50 and p99 (the client's own delay between calls would push it above).

    tools/usl_fit.py results/<date>_concurrency/summary.txt [figure.png]
    tools/usl_fit.py --self-check
"""
import json
import math
import statistics as st
import sys

import numpy as np
from scipy.optimize import curve_fit

LABELS = {"allow_all": "gRPC alone", "strict": "strict (Redis per check)", "leased": "leased (lease hits)"}
COLORS = {"allow_all": "#2a78d6", "strict": "#eb6834", "leased": "#1baf7a"}  # slots 1 to 3, validated
MARKERS = {"allow_all": "o", "strict": "s", "leased": "^"}


def usl(n, lam, sigma, kappa):
    return lam * n / (1 + sigma * (n - 1) + kappa * n * (n - 1))


def fit(points):
    n = np.array([p[0] for p in points], float)
    x = np.array([p[1] for p in points], float)
    (lam, sigma, kappa), _ = curve_fit(usl, n, x, p0=[x[n == n.min()].mean(), 0.05, 1e-4],
                                       bounds=([0, 0, 0], [np.inf, 1, 1]), maxfev=20000)
    pred = usl(n, lam, sigma, kappa)
    r2 = 1 - ((x - pred) ** 2).sum() / ((x - x.mean()) ** 2).sum()
    peak = math.sqrt((1 - sigma) / kappa) if kappa > 0 else math.inf
    return lam, sigma, kappa, r2, peak, float(np.median(np.abs(pred / x - 1)))


def load(path):
    rows = {}
    for line in open(path):
        if line.startswith("{"):
            d = json.loads(line)
            rows.setdefault(d["engine"], []).append((d["n"], d["result"]))
    return rows


def report(rows):
    out = ["| Engine | lambda (fit) | X(1) measured | sigma | kappa | R² | median abs. rel. error | Peak N* | "
           "Ceiling (fit) | Max measured |", "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |"]
    fits = {}
    for e, rs in rows.items():
        lam, sigma, kappa, r2, peak, rel = fits[e] = fit([(n, r["checks_per_s"]) for n, r in rs])
        med = {n: st.median(r["checks_per_s"] for m, r in rs if m == n) for n in {n for n, _ in rs}}
        nmax = max(med)
        no_peak = not math.isfinite(peak) or peak > 10 * nmax  # kappa about 0: no retrograde region in range
        ceiling = lam / sigma if no_peak else usl(peak, lam, sigma, kappa)
        out.append(f"| {LABELS.get(e, e)} | {lam:,.0f} | {med[min(med)]:,.0f} | {sigma:.4f} | {kappa:.1e} | {r2:.3f} | "
                   f"{100 * rel:.0f}% | {'none (kappa about 0)' if no_peak else f'{peak:,.0f}'} | {ceiling:,.0f} | "
                   f"{med[nmax]:,.0f} |")
    out += ["", "Little's law, N / X against the measured latency (medians of the runs):", "",
            "| Engine | N | X (checks/s) | N / X (ms) | p50 (ms) | p99 (ms) | p50 <= N/X <= p99 |",
            "| --- | --- | --- | --- | --- | --- | --- |"]
    for e, rs in rows.items():
        for n in sorted({n for n, _ in rs}):
            pts = [r for m, r in rs if m == n]
            x = st.median(r["checks_per_s"] for r in pts)
            p50, p99 = st.median(r["p50_ms"] for r in pts), st.median(r["p99_ms"] for r in pts)
            mean = 1000 * n / x
            out.append(f"| {LABELS.get(e, e)} | {n} | {x:,.0f} | {mean:.3f} | {p50:.3f} | {p99:.3f} | "
                       f"{'yes' if p50 * 0.9 <= mean <= p99 else 'NO'} |")
    return "\n".join(out) + "\n", fits


def plot(rows, fits, out):
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    surface, text, muted, grid = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
    plt.rcParams.update({"font.size": 10, "text.color": text, "axes.labelcolor": text,
                         "xtick.color": muted, "ytick.color": muted})
    fig, ax = plt.subplots(figsize=(7.5, 4.2), facecolor=surface)
    ax.set_facecolor(surface)
    ax.grid(axis="y", color=grid, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(grid)
    nmax = max(n for rs in rows.values() for n, _ in rs)
    curve = np.geomspace(1, nmax, 200)
    for e in ("allow_all", "leased", "strict"):
        if e not in rows:
            continue
        rs = rows[e]
        ax.plot([n for n, _ in rs], [r["checks_per_s"] / 1000 for _, r in rs], MARKERS[e], color=COLORS[e],
                markersize=6, markeredgecolor=surface, markeredgewidth=1)
        lam, sigma, kappa = fits[e][:3]
        y = usl(curve, lam, sigma, kappa) / 1000
        ax.plot(curve, y, color=COLORS[e], linewidth=2, marker=MARKERS[e], markevery=[], label=LABELS[e])
        nudge = {"allow_all": 8, "leased": -8}.get(e, 0)  # the two top curves end together
        ax.annotate(LABELS[e], (curve[-1], y[-1]), xytext=(6, nudge), textcoords="offset points", va="center",
                    color=text, fontsize=9)
    ax.set_xscale("log", base=2)
    ticks = [1, 2, 4, 8, 16, 32, 64, 128, 384]
    ax.set_xticks(ticks)
    ax.set_xticklabels([str(t) for t in ticks])
    ax.set_xlim(0.8, nmax * 2.6)
    ax.set_ylim(0, None)
    ax.set_xlabel("Calls in flight, N (closed loop, one instance)")
    ax.set_ylabel("Throughput (thousand checks/s)")
    from matplotlib.lines import Line2D

    handles = [Line2D([], [], color=COLORS[e], marker=MARKERS[e], linewidth=2, markersize=6, label=LABELS[e])
               for e in ("allow_all", "leased", "strict") if e in rows]
    ax.legend(handles=handles, frameon=False, fontsize=8, loc="upper left",
              title="points: runs; lines: USL fit", title_fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=200, facecolor=surface)


def self_check():
    rng = np.random.default_rng(1)
    pts = [(n, usl(n, 5000, 0.03, 2e-4) * (1 + rng.normal(0, 0.01))) for n in (1, 2, 4, 8, 16, 32, 64, 128, 256)
           for _ in range(3)]
    lam, sigma, kappa, r2, peak, rel = fit(pts)
    assert abs(lam / 5000 - 1) < 0.05 and abs(sigma - 0.03) < 0.01 and abs(kappa / 2e-4 - 1) < 0.2 and r2 > 0.99 and rel < 0.02, \
        (lam, sigma, kappa, r2)
    assert abs(peak - math.sqrt(0.97 / 2e-4)) / peak < 0.15
    rows = {"leased": [(n, {"checks_per_s": x, "p50_ms": 1000 * n / x * 0.95, "p99_ms": 1000 * n / x * 3})
                       for n, x in pts]}
    assert "NO" not in report(rows)[0]
    print("usl_fit self-check: ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-check"]:
        self_check()
    else:
        rows = load(sys.argv[1])
        text, fits = report(rows)
        sys.stdout.write(text)
        if len(sys.argv) > 2:
            plot(rows, fits, sys.argv[2])
