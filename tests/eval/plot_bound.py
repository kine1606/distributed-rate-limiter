#!/usr/bin/env python3
"""Figure for evaluation tests 4 and 12: over-admission against the chunk cap, measured (median and
range of the runs) and predicted by tests/eval/bound_model.py, next to the worst case N x C.

    tests/eval/plot_bound.py <results/<date>_burst_accuracy/summary.txt> <out.png>
"""
import json
import statistics
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

SURFACE, TEXT, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#e4e3df"
MEASURED, MODEL = "#2a78d6", "#eb6834"  # categorical slots 1 and 2, validated (dataviz validate_palette.js)
TITLES = {"idle_burst": "Idle, then a burst (the worst case)", "steady": "Steady overload"}


def main(summary, out):
    rows = [json.loads(line) for line in open(summary) if line.startswith("{")]
    caps = sorted({r["chunk"] for r in rows})
    pct = {r["chunk"]: r["pct"] for r in rows}
    worst = {r["chunk"]: r["worst"] for r in rows}
    plt.rcParams.update({"font.size": 10, "text.color": TEXT, "axes.labelcolor": TEXT,
                         "xtick.color": MUTED, "ytick.color": MUTED})
    fig, axes = plt.subplots(1, 2, figsize=(9, 3.8), sharey=True, facecolor=SURFACE)
    for ax, scenario in zip(axes, ("idle_burst", "steady")):
        ax.set_facecolor(SURFACE)
        ax.grid(axis="y", color=GRID, linewidth=0.8)
        ax.set_axisbelow(True)
        for side in ("top", "right"):
            ax.spines[side].set_visible(False)
        for side in ("left", "bottom"):
            ax.spines[side].set_color(GRID)
        ax.plot(caps, [worst[c] for c in caps], color=MUTED, linestyle="--", linewidth=2, label="worst case N × C")
        measured = [[r["measured_over"] for r in rows if r["scenario"] == scenario and r["chunk"] == c] for c in caps]
        model = [statistics.median(r["predicted_over"]["1ms"] for r in rows if r["scenario"] == scenario and r["chunk"] == c)
                 for c in caps]
        med = [statistics.median(m) for m in measured]
        err = [[m - min(v) for m, v in zip(med, measured)], [max(v) - m for m, v in zip(med, measured)]]
        ax.errorbar(caps, med, yerr=err, fmt="o", markersize=8, color=MEASURED, ecolor=MEASURED, elinewidth=2,
                    capsize=4, label="measured (median, range of runs)", zorder=3)
        ax.plot(caps, model, linestyle="none", marker="D", markersize=11, markerfacecolor="none",
                markeredgecolor=MODEL, markeredgewidth=2, label="model prediction", zorder=4)
        for c, m in zip(caps, med):  # selective direct labels: the measured medians
            ax.annotate(f"{m:.1f}".replace("-", "\u2212"), (c, m), textcoords="offset points",
                        xytext=(10, 8 if scenario == "steady" else -4), color=TEXT, fontsize=9)
        ax.set_title(TITLES[scenario], color=TEXT, fontsize=11, loc="left")
        ax.set_xticks(caps, [f"{c}\n({pct[c]}%)" for c in caps])
        ax.set_xlabel("Chunk cap C, tokens (% of the limit of 1,000)")
        ax.set_xlim(0, max(caps) * 1.12)
    axes[0].set_ylabel("Over-admission, tokens")
    axes[0].set_ylim(-20, max(worst.values()) * 1.1)
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, frameon=False, bbox_to_anchor=(0.5, 1.01))
    fig.tight_layout(rect=(0, 0, 1, 0.92))
    fig.savefig(out, dpi=200, facecolor=SURFACE)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
