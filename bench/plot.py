#!/usr/bin/env python3
"""Turn the CSV files written by ./bench into PNG charts.
usage: python3 bench/plot.py docs/bench_results docs
"""
import csv
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

src = sys.argv[1] if len(sys.argv) > 1 else "docs/bench_results"
out = sys.argv[2] if len(sys.argv) > 2 else "docs"


def rows(name):
    with open(f"{src}/{name}.csv") as f:
        return list(csv.DictReader(f))


# 1. write throughput
w = rows("writes")
fig, ax = plt.subplots(figsize=(5, 3.5))
labels = ["fsync on\n(durable)", "fsync off\n(not durable)"]
vals = [float(r["ops_per_sec"]) for r in w]
bars = ax.bar(labels, vals, color=["#3b6ea5", "#9bb7d4"])
ax.set_yscale("log")
ax.set_ylabel("writes per second (log scale)")
ax.set_title("Write throughput, 100 byte values")
for b, v in zip(bars, vals):
    ax.text(b.get_x() + b.get_width() / 2, v * 1.1, f"{v:,.0f}", ha="center")
ax.set_ylim(top=max(vals) * 4)
fig.tight_layout()
fig.savefig(f"{out}/bench_writes.png", dpi=130)

# 2. read latency before/after compaction
r = rows("reads")
fig, ax = plt.subplots(figsize=(6, 3.5))
groups = [("hit", "key exists"), ("miss", "key missing")]
x = 0
ticks, tick_labels = [], []
for lookup, text in groups:
    for phase, color in (("before", "#c0504d"), ("after", "#4f9a5c")):
        row = next(q for q in r if q["lookup"] == lookup and q["phase"] == phase)
        v = float(row["mean_us"])
        ax.bar(x, v, color=color, label=f"{phase} compaction" if lookup == "hit" else None)
        ax.text(x, v * 1.05, f"{v:.1f}", ha="center")
        x += 1
    ticks.append(x - 1.5)
    tick_labels.append(text)
    x += 0.6
ax.set_xticks(ticks)
ax.set_xticklabels(tick_labels)
ax.set_ylabel("mean Get latency (microseconds)")
ax.set_ylim(top=ax.get_ylim()[1] * 1.12)
ax.set_title("Read latency: 87 SSTables vs 1 SSTable")
ax.legend()
fig.tight_layout()
fig.savefig(f"{out}/bench_reads.png", dpi=130)

# 3. recovery time vs WAL size
c = rows("recovery")
fig, ax = plt.subplots(figsize=(5, 3.5))
xs = [float(q["wal_mb"]) for q in c]
ys = [float(q["recovery_seconds"]) for q in c]
ax.plot(xs, ys, marker="o", color="#3b6ea5")
for a, b in zip(xs, ys):
    ax.annotate(f"{b:.3f}s", (a, b), textcoords="offset points", xytext=(5, 6))
ax.set_xlabel("WAL size (MB)")
ax.set_ylabel("recovery time (seconds)")
ax.set_title("Recovery time grows linearly with WAL size")
ax.grid(alpha=0.3)
fig.tight_layout()
fig.savefig(f"{out}/bench_recovery.png", dpi=130)
print("charts written to", out)
