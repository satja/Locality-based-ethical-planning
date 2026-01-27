#!/usr/bin/env bash
set -euo pipefail

# Benchmark the locality planner and two baselines on *.input-only.txt and emit a CSV.
# Usage:
#   ./benchmark.sh [--dir tests] [--L 3] [--max-depth 80] [--planner-timeout 10] [--bf-timeout 30] [--auto-timeout 30] [--limit 0] [--out tests/benchmark.csv]
# Flags:
#   --dir DIR: directory containing *.input-only.txt (default: tests)
#   --L L: locality parameter forwarded to ./planner --L (default: 3)
#   --max-depth D: depth bound for baselines (default: 80)
#   --planner-timeout S: timeout seconds for locality planner (default: 10)
#   --bf-timeout S: timeout seconds for brute-force baseline (default: 30)
#   --auto-timeout S: timeout seconds for automata/progression baseline (default: 30)
#   --limit K: cap number of cases; 0 means no cap (default: 0)
#   --out FILE: output CSV path (default: tests/benchmark.csv)

DIR="tests"
L=3
MAX_DEPTH=80
PLANNER_TIMEOUT=10
BF_TIMEOUT=30
AUTO_TIMEOUT=30
LIMIT=0
OUT="tests/benchmark.csv"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR="$2"; shift 2;;
    --L)
      L="$2"; shift 2;;
    --max-depth)
      MAX_DEPTH="$2"; shift 2;;
    --planner-timeout)
      PLANNER_TIMEOUT="$2"; shift 2;;
    --bf-timeout)
      BF_TIMEOUT="$2"; shift 2;;
    --auto-timeout)
      AUTO_TIMEOUT="$2"; shift 2;;
    --limit)
      LIMIT="$2"; shift 2;;
    --out)
      OUT="$2"; shift 2;;
    *)
      echo "Unknown arg: $1" >&2; exit 2;;
  esac
done

mkdir -p "$DIR"

# Parse manifest into associative maps: case -> n, case -> broken_count.
declare -A N_BY_CASE
declare -A BROKEN_BY_CASE
if [[ -f "$DIR/manifest.txt" ]]; then
  while read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" == seed=* ]] && continue
    case_name=$(awk '{print $1}' <<<"$line")
    n_val=$(sed -n 's/.* n=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    broken_field=$(sed -n 's/.* broken=\(.*\)$/\1/p' <<<"$line")
    if [[ -z "$broken_field" ]]; then
      broken_cnt=0
    elif [[ "$broken_field" == "" ]]; then
      broken_cnt=0
    else
      # Count comma-separated entries; handle empty string as 0.
      if [[ "$broken_field" == "" ]]; then
        broken_cnt=0
      elif [[ "$broken_field" == *","* ]]; then
        broken_cnt=$(( $(tr -cd ',' <<<"$broken_field" | wc -c) + 1 ))
      elif [[ "$broken_field" == "" ]]; then
        broken_cnt=0
      else
        # single value or literal empty
        if [[ "$broken_field" == "" ]]; then broken_cnt=0; else broken_cnt=1; fi
      fi
    fi
    if [[ -n "$case_name" && -n "$n_val" ]]; then
      N_BY_CASE["$case_name"]="$n_val"
      BROKEN_BY_CASE["$case_name"]="$broken_cnt"
    fi
  done < "$DIR/manifest.txt"
fi

run_timed() {
  local input_file="$1"; shift
  local output_file="$1"; shift
  local timeout_secs="$1"; shift
  local err_file="${output_file}.err"
  local tmp_time
  tmp_time=$(mktemp)
  set +e
  if [[ "$timeout_secs" -gt 0 ]]; then
    /usr/bin/time -q -f "%e" -o "$tmp_time" \
      /usr/bin/timeout --signal=TERM --kill-after=1s "${timeout_secs}s" \
      "$@" < "$input_file" > "$output_file" 2> "$err_file"
  else
    /usr/bin/time -q -f "%e" -o "$tmp_time" \
      "$@" < "$input_file" > "$output_file" 2> "$err_file"
  fi
  local status=$?
  set -e
  local elapsed
  elapsed=$(cat "$tmp_time" 2>/dev/null || echo "nan")
  # Normalize timeout cases to the requested limit for clearer plots/CSVs.
  if [[ "$timeout_secs" -gt 0 && ( "$status" -eq 124 || "$status" -eq 137 ) ]]; then
    elapsed="$timeout_secs"
  fi
  rm -f "$tmp_time"
  echo "$status,$elapsed"
}

echo "case,n,broken_count,planner_status,planner_time_s,bruteforce_status,bruteforce_time_s,automata_status,automata_time_s" > "$OUT"

shopt -s nullglob
case_count=0
# Prefer systematic ordering by size when a manifest is available.
FILES=()
if [[ -f "$DIR/manifest.txt" ]]; then
  mapfile -t CASES < <(
    awk 'NF && $1 !~ /^seed=/ {print}' "$DIR/manifest.txt" \
    | sed -n 's/^\([^ ]*\).* n=\([0-9][0-9]*\).*/\2 \1/p' \
    | sort -n -k1,1 -k2,2 \
    | awk '{print $2}'
  )
  for case_name in "${CASES[@]}"; do
    FILES+=("$DIR/$case_name")
  done
elif [[ "$DIR" == "." ]]; then
  mapfile -t FILES < <(find . -type f -name "*.input-only.txt" -not -path "./deprecated/*" | sort)
else
  FILES=("$DIR"/*.input-only.txt)
fi

for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || continue
  if [[ "$LIMIT" -gt 0 && "$case_count" -ge "$LIMIT" ]]; then
    break
  fi
  case_name=$(basename "$f")
  n_val=${N_BY_CASE[$case_name]:-""}
  broken_cnt=${BROKEN_BY_CASE[$case_name]:-""}

  planner_out="${f%.input-only.txt}.paper.full.txt"
  bf_out="${f%.input-only.txt}.bf.full.txt"
  auto_out="${f%.input-only.txt}.auto.full.txt"

  IFS=',' read -r planner_status planner_time < <(run_timed "$f" "$planner_out" "$PLANNER_TIMEOUT" ./planner --L "$L")
  IFS=',' read -r bf_status bf_time < <(run_timed "$f" "$bf_out" "$BF_TIMEOUT" ./bruteforce-planner --max-depth "$MAX_DEPTH")
  IFS=',' read -r auto_status auto_time < <(run_timed "$f" "$auto_out" "$AUTO_TIMEOUT" ./ltlf-progress-planner --max-depth "$MAX_DEPTH")

  echo "$case_name,$n_val,$broken_cnt,$planner_status,$planner_time,$bf_status,$bf_time,$auto_status,$auto_time" >> "$OUT"
  echo "benchmarked $case_name (planner ${planner_time}s, bf ${bf_time}s, auto ${auto_time}s)"
  case_count=$((case_count + 1))
done

# Record the configuration used for the benchmark.
echo "# L=$L max_depth=$MAX_DEPTH planner_timeout=$PLANNER_TIMEOUT bf_timeout=$BF_TIMEOUT auto_timeout=$AUTO_TIMEOUT limit=$LIMIT" >> "$OUT"
echo "Wrote $OUT"
