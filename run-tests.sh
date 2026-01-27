#!/usr/bin/env bash
set -u

# Batch test driver:
# - Generates full validator inputs using planner(s).
# - Validates them with the independent validator.
# - Avoids deprecated/ by default even if DIR=".".
# Usage:
#   ./run-tests.sh [paper|bruteforce|automata|both|all] [--dir DIR] [--mode MODE] [--planner-timeout S] [--bf-timeout S] [--auto-timeout S]
# Flags:
#   --dir DIR: directory containing *.input-only.txt (default: tests_systematic)
#   --mode MODE: one of paper|bruteforce|automata|both|all
#   --planner-timeout S: seconds for planner/validate in paper mode (default: 10)
#   --bf-timeout S: seconds for bruteforce baseline/validate (default: 30)
#   --auto-timeout S: seconds for automata baseline/validate (default: 30)

L=${L:-3}
DIR=${DIR:-tests_systematic}
MODE=paper  # paper | bruteforce | automata | both | all
PLANNER_TIMEOUT=${PLANNER_TIMEOUT:-10}
BF_TIMEOUT=${BF_TIMEOUT:-30}
AUTO_TIMEOUT=${AUTO_TIMEOUT:-30}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR="$2"; shift 2;;
    --mode)
      MODE="$2"; shift 2;;
    --planner-timeout)
      PLANNER_TIMEOUT="$2"; shift 2;;
    --bf-timeout)
      BF_TIMEOUT="$2"; shift 2;;
    --auto-timeout)
      AUTO_TIMEOUT="$2"; shift 2;;
    paper|bruteforce|automata|both|all)
      MODE="$1"; shift;;
    *)
      echo "Unknown arg: $1" >&2
      echo "Usage: ./run-tests.sh [paper|bruteforce|automata|both|all] [--dir DIR] [--mode MODE] [--planner-timeout S] [--bf-timeout S] [--auto-timeout S]" >&2
      exit 2;;
  esac
done

paper_ok=0
paper_fail=0
bf_ok=0
bf_fail=0
auto_ok=0
auto_fail=0

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

run_with_timeout() {
  local timeout_secs="$1"; shift
  if [[ "$timeout_secs" -gt 0 ]]; then
    /usr/bin/timeout --signal=TERM --kill-after=1s "${timeout_secs}s" "$@"
  else
    "$@"
  fi
}

for f in "${FILES[@]}"; do
  base=${f%.input-only.txt}

  if [[ "$MODE" == "paper" || "$MODE" == "both" || "$MODE" == "all" ]]; then
    out="${base}.paper.full.txt"
    if run_with_timeout "$PLANNER_TIMEOUT" ./planner --L "$L" < "$f" > "$out" \
      && run_with_timeout "$PLANNER_TIMEOUT" ./validate < "$out"; then
      paper_ok=$((paper_ok + 1))
      echo "[paper] OK   $f"
    else
      paper_fail=$((paper_fail + 1))
      echo "[paper] FAIL $f"
    fi
  fi

  if [[ "$MODE" == "bruteforce" || "$MODE" == "both" || "$MODE" == "all" ]]; then
    out="${base}.bf.full.txt"
    if run_with_timeout "$BF_TIMEOUT" ./bruteforce-planner --max-depth 80 < "$f" > "$out" \
      && run_with_timeout "$BF_TIMEOUT" ./validate < "$out"; then
      bf_ok=$((bf_ok + 1))
      echo "[bf]    OK   $f"
    else
      bf_fail=$((bf_fail + 1))
      echo "[bf]    FAIL $f"
    fi
  fi

  if [[ "$MODE" == "automata" || "$MODE" == "all" ]]; then
    out="${base}.auto.full.txt"
    if run_with_timeout "$AUTO_TIMEOUT" ./ltlf-progress-planner --max-depth 80 < "$f" > "$out" \
      && run_with_timeout "$AUTO_TIMEOUT" ./validate < "$out"; then
      auto_ok=$((auto_ok + 1))
      echo "[auto]  OK   $f"
    else
      auto_fail=$((auto_fail + 1))
      echo "[auto]  FAIL $f"
    fi
  fi
done

echo
echo "L=$L mode=$MODE dir=$DIR"
echo "timeouts: planner=$PLANNER_TIMEOUT bf=$BF_TIMEOUT auto=$AUTO_TIMEOUT"
if [[ "$MODE" == "paper" || "$MODE" == "both" || "$MODE" == "all" ]]; then
  echo "paper: ok=$paper_ok fail=$paper_fail"
fi
if [[ "$MODE" == "bruteforce" || "$MODE" == "both" || "$MODE" == "all" ]]; then
  echo "bruteforce: ok=$bf_ok fail=$bf_fail"
fi
if [[ "$MODE" == "automata" || "$MODE" == "all" ]]; then
  echo "automata: ok=$auto_ok fail=$auto_fail"
fi
