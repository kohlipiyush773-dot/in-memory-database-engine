#!/usr/bin/env python3
"""
Draws Matplotlib graphs from the benchmark results.

  1. run the benchmark once in the web UI (Benchmark tab)  -> results/bench_latest.json is written
  2. python tools/plot_benchmarks.py                        -> PNG files in results/

Needs:  pip install matplotlib
"""
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "results", "bench_latest.json")
COLORS = {"FIFO": "#f59e0b", "LRU": "#0ea5e9", "CLOCK": "#8b5cf6", "OPT": "#10b981"}


def main():
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        sys.exit("matplotlib is not installed:  pip install matplotlib")
    if not os.path.isfile(SRC):
        sys.exit("run the benchmark in the web UI first (it creates %s)" % SRC)
    data = json.load(open(SRC))
    rows = data["results"]
    workloads = sorted({r["w"] for r in rows})
    policies = [p for p in ("FIFO", "LRU", "CLOCK", "OPT") if any(r["p"] == p for r in rows)]
    sizes = sorted({r["f"] for r in rows})
    out = os.path.join(ROOT, "results")

    fig, axes = plt.subplots(1, len(workloads), figsize=(4.6 * len(workloads), 3.8), squeeze=False)
    for ax, w in zip(axes[0], workloads):
        for p in policies:
            ys = [next(r["hit_ratio"] for r in rows if r["w"] == w and r["p"] == p and r["f"] == s) * 100 for s in sizes]
            ax.plot(sizes, ys, marker="o", label=p, color=COLORS[p], linestyle="--" if p == "OPT" else "-")
        ax.set_title(w)
        ax.set_xlabel("buffer pool size (frames)")
        ax.set_xscale("log", base=2)
        ax.set_xticks(sizes)
        ax.set_xticklabels(sizes)
        ax.set_ylim(0, 100)
        ax.grid(alpha=0.3)
    axes[0][0].set_ylabel("hit ratio (%)")
    axes[0][0].legend()
    fig.suptitle("Buffer pool hit ratio (database = %d pages, %d accesses, %d%% writes)" % (
        data["config"]["pages"], data["config"]["ops"], data["config"]["write_pct"]))
    fig.tight_layout()
    fig.savefig(os.path.join(out, "hit_ratio.png"), dpi=140)

    mid = sizes[min(3, len(sizes) - 1)]
    fig2, axes2 = plt.subplots(1, 2, figsize=(10, 3.8))
    width = 0.8 / len(policies)
    for k, key in enumerate(("reads", "writes")):
        for i, p in enumerate(policies):
            vals = [next(r[key] for r in rows if r["w"] == w and r["p"] == p and r["f"] == mid) for w in workloads]
            axes2[k].bar([x + i * width for x in range(len(workloads))], vals, width, label=p, color=COLORS[p])
        axes2[k].set_xticks([x + 0.4 - width / 2 for x in range(len(workloads))])
        axes2[k].set_xticklabels(workloads)
        axes2[k].set_title("disk %s at %d frames" % (key, mid))
        axes2[k].grid(axis="y", alpha=0.3)
    axes2[0].legend()
    fig2.tight_layout()
    fig2.savefig(os.path.join(out, "disk_io.png"), dpi=140)
    print("wrote", os.path.join(out, "hit_ratio.png"), "and", os.path.join(out, "disk_io.png"))


if __name__ == "__main__":
    main()
