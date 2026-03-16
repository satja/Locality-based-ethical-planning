#!/usr/bin/env python3

import argparse
import csv
import math
import statistics
from collections import Counter
from pathlib import Path


def read_rows(path: Path):
    rows = []
    with path.open(newline="") as f:
        filtered = [line for line in f if line.strip() and not line.startswith("#")]
    for row in csv.DictReader(filtered):
        rows.append(row)
    return rows


def times(rows, prefix):
    vals = []
    for r in rows:
        try:
            if int(r[f"{prefix}_status"]) == 0:
                vals.append(float(r[f"{prefix}_time_s"]))
        except Exception:
            pass
    return vals


def ints(rows, field):
    vals = []
    for r in rows:
        try:
            if r[field] != "" and r[field] != "na":
                vals.append(int(r[field]))
        except Exception:
            pass
    return vals


def summarize(label, vals):
    if not vals:
        return f"{label}: none"
    return (
        f"{label}: count={len(vals)} mean={statistics.mean(vals):.3f} "
        f"median={statistics.median(vals):.3f} max={max(vals):.3f}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize locality-variant benchmark CSVs.")
    parser.add_argument("--csv", required=True, help="CSV produced by benchmark-locality-variants.sh")
    args = parser.parse_args()

    rows = read_rows(Path(args.csv))
    print(f"rows={len(rows)}")
    for prefix in ("arbitrary", "shortest", "conflict"):
        ok = sum(1 for r in rows if r.get(f"{prefix}_status") == "0")
        timeout = sum(1 for r in rows if r.get(f"{prefix}_status") in {"124", "137"})
        print(f"{prefix}: success={ok}/{len(rows)} timeout={timeout}")
        print("  " + summarize("time", times(rows, prefix)))

    print("  " + summarize("arbitrary length", ints(rows, "arbitrary_plan_length")))
    print("  " + summarize("shortest length", ints(rows, "shortest_plan_length")))
    print("  " + summarize("conflict length", ints(rows, "conflict_plan_length")))

    penalties = ints(rows, "conflict_penalty")
    if penalties:
        print("  " + summarize("conflict penalty", penalties))
        print(f"  conflict penalty histogram={dict(sorted(Counter(penalties).items()))}")

    scored = ints(rows, "conflict_score_penalty")
    if scored:
        print("  " + summarize("scored conflict penalty", scored))


if __name__ == "__main__":
    main()
