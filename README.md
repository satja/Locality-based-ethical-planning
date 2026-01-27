# LTL Planner + Validator

This repo contains three separate programs:
- `planner.cpp`: the paper-aligned planner (fast, intended algorithm).
- `bruteforce-planner.cpp`: a brute-force baseline (slow, with a timeout).
- `validate.cpp`: the independent validator (checks correctness only).

There are also scripts for systematic test generation, benchmarking, and plotting.

## 0) Approaches and Comparisons (Journal-Ready Summary)

This repo now supports three clearly distinct planning approaches for comparison:

- Paper planner (`planner.cpp`): Implements the locality-based “graph of substates” algorithm from the paper. It uses the locality parameter `L` to restrict what can still change and prunes using validity checks over a moving window.
- Brute-force with step pruning (`bruteforce-planner.cpp`): Explores full valuations plus temporal memory and prunes immediately if any value is violated at an intermediate step. This is a strong but still-local baseline that enforces values “online.”
- Final-only brute-force baseline (`bruteforce-planner.cpp --final-only`): Explores the same full state, but does not prune on intermediate value violations. It only checks satisfaction at the final step. This more closely reflects a generic bounded search that lacks the paper’s pruning structure and is useful to show search blow-up.

These three modes give you a principled “algorithm vs baselines” story without introducing extra external tooling.

## 1) Basic Setup: Build, Run, Validate

Build all programs:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic planner.cpp -o planner
g++ -std=c++17 -O2 -Wall -Wextra -pedantic bruteforce-planner.cpp -o bruteforce-planner
g++ -std=c++17 -O2 -Wall -Wextra -pedantic validate.cpp -o validate
```

Use the input-only format as planner input, and the full format as validator input.

Single-case workflow:

```bash
./planner --L 3 < input-only.txt > planned.paper.full.txt
./validate < planned.paper.full.txt
```

With systematic tests:

```bash
./planner --L 3 < tests_systematic/case-0.input-only.txt > tests_systematic/case-0.paper.full.txt
./validate < tests_systematic/case-0.paper.full.txt
```

Notes:
- The planners do not perform validation. Only `validate` checks correctness.
- The planner output is intentionally in the validator’s full input format.
- `bruteforce-planner` supports `--final-only` to disable intermediate pruning.

## 2) Test Generation: Systematic, Publishable, Parameterized

The recommended generator is `gen-systematic-tests.cpp`. It creates deterministic,
size-ordered families of traffic-light-like instances that match the paper’s
structure.

Build the generator:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic gen-systematic-tests.cpp -o gen-systematic-tests
```

Generate a systematic suite:

```bash
./gen-systematic-tests --dir tests_systematic --n-min 6 --n-max 24 --per-n 3 --seed 2026
```

What it generates:
- A family of ring-structured instances indexed by `n`.
- Local atoms per light: `on_i`, `broken_i`.
- A global atom: `congestion`.
- Transition rules (`gamma`) aligned with the paper’s definitions.
- Values typically include `F G on_i` (for each light) and `G !congestion`.
- Broken-light patterns are chosen systematically (non-adjacent where possible).

Files produced (per case):
- `*.input-only.txt`: planner input.
- `manifest.txt`: metadata with `n` and broken pattern per case.

Generator parameters:
- `--dir DIR`: output directory.
- `--n-min N`: smallest size `n`.
- `--n-max N`: largest size `n`.
- `--per-n K`: number of cases per size.
- `--seed S`: seed for reproducibility.

Deprecated random tests and the old generator live under `deprecated/`.

## 3) Running Benchmarks and Generating Charts

### Benchmarking

Use `benchmark.sh` to run the paper planner and two baselines with timeouts and produce a CSV.

Example (paper planner under 10s, baselines under 30s):

```bash
./benchmark.sh \
  --dir tests_systematic \
  --L 3 \
  --max-depth 160 \
  --planner-timeout 10 \
  --bf-timeout 30 \
  --final-timeout 30 \
  --out tests_systematic/benchmark.csv
```

Key parameters:
- `--dir DIR`: where `*.input-only.txt` live.
- `--L L`: paper planner lookahead/limit parameter.
- `--max-depth D`: brute-force depth bound.
- `--planner-timeout T`: planner timeout in seconds.
- `--bf-timeout T`: brute-force timeout in seconds.
- `--final-timeout T`: final-only baseline timeout in seconds.
- `--limit K`: optional cap on number of cases.
- `--out FILE`: output CSV path.

Behavior:
- Cases are sorted by size `n` when `manifest.txt` is present.
- If `--dir .` is used, `deprecated/` is excluded automatically.
- The CSV now includes both baselines: `bruteforce_*` and `final_only_*`.
- Timeout statuses are typically `124` or `137` (normalized to the timeout value for clearer plots).

### Plotting

Use `plot-benchmarks.py` to generate plots from the CSV:

```bash
python3 plot-benchmarks.py --csv tests_systematic/benchmark.csv
```

This writes plots next to the CSV (by default):
- `tests_systematic/benchmark-time-vs-n.png`
- `tests_systematic/benchmark-time-vs-case.png`

If needed, you can choose a different output folder:

```bash
python3 plot-benchmarks.py --csv tests_systematic/benchmark.csv --out-dir tests_systematic
```

### Opening plots from the terminal

On Ubuntu:

```bash
xdg-open tests_systematic/benchmark-time-vs-n.png
xdg-open tests_systematic/benchmark-time-vs-case.png
```

## Other Helpful Commands

Batch validation with the run script:

```bash
./run-tests.sh paper --dir tests_systematic
./run-tests.sh both --dir tests_systematic
./run-tests.sh all --dir tests_systematic
```

If you point it at `--dir .`, it will skip `deprecated/`.

The test runner now supports per-approach timeouts:

```bash
./run-tests.sh all --dir tests_systematic \
  --planner-timeout 10 \
  --bf-timeout 5 \
  --final-timeout 5
```

## 4) Experiments You Can Report

Below are concrete, reproducible experiments that align well with the paper’s claims.

### A. Main comparison: algorithm vs baselines

Goal:
- Show that the paper planner scales on locality-structured instances where generic bounded search times out.

Recommended command:

```bash
./benchmark.sh \
  --dir tests_systematic \
  --L 3 \
  --max-depth 80 \
  --planner-timeout 10 \
  --bf-timeout 5 \
  --final-timeout 5 \
  --out tests_systematic/benchmark.csv
python3 plot-benchmarks.py --csv tests_systematic/benchmark.csv
```

What to report:
- Success rates by approach (status `0` vs timeout).
- Runtime vs `n`.
- The gap between step-pruned brute force and final-only brute force helps isolate the value of “online pruning” vs the paper’s locality structure.

### B. Locality ablation over `L`

Goal:
- Demonstrate the trade-off between pruning power and flexibility as `L` changes.

Suggested sweep:
- Repeat the benchmark for `L in {2, 3, 4, 5}` and compare curves.

Example command:

```bash
for L in 2 3 4 5; do
  ./benchmark.sh --dir tests_systematic --L "$L" --max-depth 80 \
    --planner-timeout 10 --bf-timeout 5 --final-timeout 5 \
    --out "tests_systematic/benchmark.L${L}.csv"
done
```

### C. Depth sensitivity for baselines

Goal:
- Show that increasing the bound helps small cases but does not change the overall scaling story.

Suggested sweep:
- Run with `--max-depth` in `{40, 80, 120}` on a fixed directory and compare failure modes.

---

Implementation notes relevant to experiments:

- `benchmark.sh` writes per-case outputs next to inputs as: `*.paper.full.txt`, `*.bf.full.txt`, `*.final.full.txt`
- Plots are always derived from the CSV, so they remain reproducible.
