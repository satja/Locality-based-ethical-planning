# LTL Planner + Validator

This repo contains three separate programs:
- `planner.cpp`: the paper-aligned planner (fast, intended algorithm).
- `bruteforce-planner.cpp`: a brute-force baseline (slow, with a timeout).
- `validate.cpp`: the independent validator (checks correctness only).

There are also scripts for systematic test generation, benchmarking, and plotting.

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

Use `benchmark.sh` to run both planners with timeouts and produce a CSV.

Example (paper planner under 10s, brute force under 30s):

```bash
./benchmark.sh \
  --dir tests_systematic \
  --L 3 \
  --max-depth 160 \
  --planner-timeout 10 \
  --bf-timeout 30 \
  --out tests_systematic/benchmark.csv
```

Key parameters:
- `--dir DIR`: where `*.input-only.txt` live.
- `--L L`: paper planner lookahead/limit parameter.
- `--max-depth D`: brute-force depth bound.
- `--planner-timeout T`: planner timeout in seconds.
- `--bf-timeout T`: brute-force timeout in seconds.
- `--limit K`: optional cap on number of cases.
- `--out FILE`: output CSV path.

Behavior:
- Cases are sorted by size `n` when `manifest.txt` is present.
- If `--dir .` is used, `deprecated/` is excluded automatically.

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
```

If you point it at `--dir .`, it will skip `deprecated/`.
