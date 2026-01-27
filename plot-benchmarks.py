import csv
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import argparse

# CLI flags:
#   --csv PATH: benchmark CSV to read (default: tests/benchmark.csv)
#   --out-dir DIR: where to write plots (default: CSV directory)

parser = argparse.ArgumentParser(description="Plot planner benchmarks from a CSV.")
parser.add_argument("--csv", default="tests/benchmark.csv", help="Path to benchmark CSV.")
parser.add_argument("--out-dir", default=None, help="Directory to place plots (defaults to CSV dir).")
args = parser.parse_args()

CSV_PATH = Path(args.csv)
OUT_DIR = Path(args.out_dir) if args.out_dir else CSV_PATH.parent
OUT_DIR.mkdir(parents=True, exist_ok=True)

rows = []
with CSV_PATH.open(newline="") as f:
    for line in f:
        if line.startswith("#") or not line.strip():
            continue
        rows.append(line)

reader = csv.DictReader(rows)

data = []
for r in reader:
    try:
        n = int(r["n"]) if r["n"] else None
        broken = int(r["broken_count"]) if r["broken_count"] else None
        planner_status = int(r["planner_status"])
        planner_time = float(r["planner_time_s"])
        bf_status = int(r["bruteforce_status"])
        bf_time = float(r["bruteforce_time_s"])
        auto_status = int(r["automata_status"]) if r.get("automata_status") else None
        auto_time = float(r["automata_time_s"]) if r.get("automata_time_s") else float("nan")
    except Exception:
        continue
    data.append(
        (
            r["case"],
            n,
            broken,
            planner_status,
            planner_time,
            bf_status,
            bf_time,
            auto_status,
            auto_time,
        )
    )

# Filter to successful runs with known n.
ok = [d for d in data if d[1] is not None]
TIMEOUT_STATUSES = {124, 137}
planner_ok = [d for d in ok if d[3] == 0]
planner_to = [d for d in ok if d[3] in TIMEOUT_STATUSES]
bf_ok = [d for d in ok if d[5] == 0]
bf_to = [d for d in ok if d[5] in TIMEOUT_STATUSES]
auto_ok = [d for d in ok if d[7] == 0]
auto_to = [d for d in ok if d[7] in TIMEOUT_STATUSES]

# ---- Plot 1: time vs n (scatter + mean line) ----
fig, ax = plt.subplots(figsize=(8, 5))

ax.scatter([d[1] for d in planner_ok], [d[4] for d in planner_ok], label="planner", alpha=0.8)
ax.scatter([d[1] for d in bf_ok], [d[6] for d in bf_ok], label="bruteforce", alpha=0.8)
if auto_ok:
    ax.scatter([d[1] for d in auto_ok], [d[8] for d in auto_ok], label="automata", alpha=0.8)
if planner_to:
    ax.scatter([d[1] for d in planner_to], [d[4] for d in planner_to],
               label="planner timeout", marker="x")
if bf_to:
    ax.scatter([d[1] for d in bf_to], [d[6] for d in bf_to],
               label="bruteforce timeout", marker="x")
if auto_to:
    ax.scatter([d[1] for d in auto_to], [d[8] for d in auto_to],
               label="automata timeout", marker="x")


def mean_by_n(points, idx_time):
    buckets = defaultdict(list)
    for d in points:
        buckets[d[1]].append(d[idx_time])
    xs = sorted(buckets)
    ys = [sum(buckets[x]) / len(buckets[x]) for x in xs]
    return xs, ys

px, py = mean_by_n(planner_ok, 4)
bx, by = mean_by_n(bf_ok, 6)
ax_x, ax_y = mean_by_n(auto_ok, 8) if auto_ok else ([], [])
if px:
    ax.plot(px, py, linestyle="--")
if bx:
    ax.plot(bx, by, linestyle="--")
if ax_x:
    ax.plot(ax_x, ax_y, linestyle="--")

ax.set_title("Planner Runtime vs n")
ax.set_xlabel("n (number of intersections)")
ax.set_ylabel("time (seconds)")
ax.legend()
ax.grid(True, alpha=0.3)

out1 = OUT_DIR / "benchmark-time-vs-n.png"
fig.tight_layout()
fig.savefig(out1, dpi=160)
plt.close(fig)

# ---- Plot 2: per-case comparison (log scale if needed) ----
cases = [d[0] for d in ok]
planner_times = [d[4] for d in ok]
bf_times = [d[6] for d in ok]
auto_times = [d[8] for d in ok] if any(d[7] is not None for d in ok) else []

fig, ax = plt.subplots(figsize=(10, 5))
ax.plot(planner_times, marker="o", label="planner")
ax.plot(bf_times, marker="o", label="bruteforce")
if auto_times:
    ax.plot(auto_times, marker="o", label="automata")

ax.set_title("Per-case Runtime Comparison")
ax.set_xlabel("case index")
ax.set_ylabel("time (seconds)")
ax.legend()
ax.grid(True, alpha=0.3)

# Use log scale when times vary a lot.
all_times = planner_times + bf_times + auto_times
max_time = max([t for t in all_times if t == t] or [0.0])
min_time = min([t for t in all_times if t == t] or [0.0])
if max_time > 0 and min_time > 0 and max_time / min_time > 50:
    ax.set_yscale("log")

out2 = OUT_DIR / "benchmark-time-vs-case.png"
fig.tight_layout()
fig.savefig(out2, dpi=160)
plt.close(fig)

print(f"Wrote {out1}")
print(f"Wrote {out2}")
