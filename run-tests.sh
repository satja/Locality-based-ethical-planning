#!/usr/bin/env bash
set -u

# Batch test driver:
# - Generates full validator inputs using planner(s).
# - Validates them with the independent validator.
# - Avoids deprecated/ by default even if DIR=".".

L=${L:-3}
DIR=${DIR:-tests_systematic}
MODE=paper  # paper | bruteforce | both

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR="$2"; shift 2;;
    --mode)
      MODE="$2"; shift 2;;
    paper|bruteforce|both)
      MODE="$1"; shift;;
    *)
      echo "Unknown arg: $1" >&2
      echo "Usage: ./run-tests.sh [paper|bruteforce|both] [--dir DIR] [--mode MODE]" >&2
      exit 2;;
  esac
done

paper_ok=0
paper_fail=0
bf_ok=0
bf_fail=0

# Build the file list. If DIR is ".", explicitly skip deprecated/.
FILES=()
if [[ "$DIR" == "." ]]; then
  mapfile -t FILES < <(find . -type f -name "*.input-only.txt" -not -path "./deprecated/*" | sort)
else
  shopt -s nullglob
  FILES=("$DIR"/*.input-only.txt)
  shopt -u nullglob
fi

if [[ ${#FILES[@]} -eq 0 ]]; then
  echo "No input-only test files found in DIR=$DIR" >&2
  exit 1
fi

for f in "${FILES[@]}"; do
  base=${f%.input-only.txt}

  if [[ "$MODE" == "paper" || "$MODE" == "both" ]]; then
    out="${base}.paper.full.txt"
    if ./planner --L "$L" < "$f" > "$out" && ./validate < "$out"; then
      paper_ok=$((paper_ok + 1))
      echo "[paper] OK   $f"
    else
      paper_fail=$((paper_fail + 1))
      echo "[paper] FAIL $f"
    fi
  fi

  if [[ "$MODE" == "bruteforce" || "$MODE" == "both" ]]; then
    out="${base}.bf.full.txt"
    if ./bruteforce-planner --max-depth 80 < "$f" > "$out" && ./validate < "$out"; then
      bf_ok=$((bf_ok + 1))
      echo "[bf]    OK   $f"
    else
      bf_fail=$((bf_fail + 1))
      echo "[bf]    FAIL $f"
    fi
  fi
done

echo
echo "L=$L mode=$MODE dir=$DIR"
if [[ "$MODE" == "paper" || "$MODE" == "both" ]]; then
  echo "paper: ok=$paper_ok fail=$paper_fail"
fi
if [[ "$MODE" == "bruteforce" || "$MODE" == "both" ]]; then
  echo "bruteforce: ok=$bf_ok fail=$bf_fail"
fi
