#!/usr/bin/env bash
set -euo pipefail

# Benchmark the three locality variants:
#   arbitrary  -> DFS satisfactory plan
#   shortest   -> shortest satisfactory plan
#   conflict   -> minimum-violation plan

DIR="tests_systematic"
L=3
MAX_DEPTH=160
TIMEOUT=15
OUT="tests_systematic/locality-variants.csv"
STRICT_VALIDATE=0
SCORE_CONFLICT=0
LIMIT=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --dir)
      DIR="$2"; shift 2;;
    --L)
      L="$2"; shift 2;;
    --max-depth)
      MAX_DEPTH="$2"; shift 2;;
    --timeout)
      TIMEOUT="$2"; shift 2;;
    --out)
      OUT="$2"; shift 2;;
    --limit)
      LIMIT="$2"; shift 2;;
    --validate)
      STRICT_VALIDATE=1; shift 1;;
    --score-conflict)
      SCORE_CONFLICT=1; shift 1;;
    *)
      echo "Unknown arg: $1" >&2; exit 2;;
  esac
done

run_timed() {
  local input_file="$1"; shift
  local output_file="$1"; shift
  local timeout_secs="$1"; shift
  local err_file="${output_file}.err"
  local tmp_time
  tmp_time=$(mktemp)
  set +e
  /usr/bin/time -q -f "%e" -o "$tmp_time" \
    /usr/bin/timeout --signal=TERM --kill-after=1s "${timeout_secs}s" \
    "$@" < "$input_file" > "$output_file" 2> "$err_file"
  local status=$?
  set -e
  local elapsed
  elapsed=$(cat "$tmp_time" 2>/dev/null || echo "nan")
  if ! [[ "$elapsed" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    elapsed="nan"
  fi
  if [[ "$status" -eq 124 || "$status" -eq 137 ]]; then
    elapsed="$timeout_secs"
  fi
  rm -f "$tmp_time"
  echo "$status,$elapsed"
}

parse_result_field() {
  local err_file="$1"
  local key="$2"
  python3 - "$err_file" "$key" <<'PY'
import re
import sys
path, key = sys.argv[1], sys.argv[2]
text = ""
try:
    text = open(path).read()
except FileNotFoundError:
    pass
m = re.search(rf"{re.escape(key)}=([^ \n\r]+)", text)
print(m.group(1) if m else "")
PY
}

validate_output() {
  local output_file="$1"
  local status
  set +e
  ./validate < "$output_file" >/dev/null 2>&1
  status=$?
  set -e
  if [[ "$status" -eq 0 ]]; then
    echo "0"
  else
    echo "1"
  fi
}

score_output() {
  local output_file="$1"
  ./score-plan < "$output_file"
}

mkdir -p "$(dirname "$OUT")"

echo "case,n,family,arbitrary_status,arbitrary_time_s,arbitrary_plan_length,shortest_status,shortest_time_s,shortest_plan_length,conflict_status,conflict_time_s,conflict_plan_length,conflict_penalty,arbitrary_valid,shortest_valid,conflict_score_penalty,conflict_score_satisfied,conflict_score_total" > "$OUT"

declare -A N_BY_CASE
if [[ -f "$DIR/manifest.txt" ]]; then
  while read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" == seed=* ]] && continue
    case_name=$(awk '{print $1}' <<<"$line")
    n_val=$(sed -n 's/.* n=\([0-9][0-9]*\).*/\1/p' <<<"$line")
    if [[ -n "$case_name" && -n "$n_val" ]]; then
      N_BY_CASE["$case_name"]="$n_val"
    fi
  done < "$DIR/manifest.txt"
fi

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
else
  FILES=("$DIR"/*.input-only.txt)
fi

shopt -s nullglob
count=0
for f in "${FILES[@]}"; do
  [[ -f "$f" ]] || continue
  if [[ "$LIMIT" -gt 0 && "$count" -ge "$LIMIT" ]]; then
    break
  fi

  case_name=$(basename "$f")
  n_val=${N_BY_CASE[$case_name]:-}
  family="unknown"
  [[ "$case_name" == *"-ex1."* ]] && family="ex1"
  [[ "$case_name" == *"-ex2."* ]] && family="ex2"
  [[ "$case_name" == *"-ex3."* ]] && family="ex3"

  arb_out="${f%.input-only.txt}.arb.full.txt"
  short_out="${f%.input-only.txt}.short.full.txt"
  conflict_out="${f%.input-only.txt}.conflict.full.txt"

  IFS=',' read -r arb_status arb_time < <(run_timed "$f" "$arb_out" "$TIMEOUT" ./planner --L "$L" --max-depth "$MAX_DEPTH")
  IFS=',' read -r short_status short_time < <(run_timed "$f" "$short_out" "$TIMEOUT" ./planner --L "$L" --max-depth "$MAX_DEPTH" --mode shortest)
  IFS=',' read -r conflict_status conflict_time < <(run_timed "$f" "$conflict_out" "$TIMEOUT" ./planner --L "$L" --max-depth "$MAX_DEPTH" --mode conflict)

  arb_len=$(parse_result_field "${arb_out}.err" "plan_length")
  short_len=$(parse_result_field "${short_out}.err" "plan_length")
  conflict_len=$(parse_result_field "${conflict_out}.err" "plan_length")
  conflict_penalty=$(parse_result_field "${conflict_out}.err" "penalty")

  arb_valid="na"
  short_valid="na"
  if [[ "$STRICT_VALIDATE" -eq 1 ]]; then
    if [[ "$arb_status" -eq 0 ]]; then
      arb_valid=$(validate_output "$arb_out")
    fi
    if [[ "$short_status" -eq 0 ]]; then
      short_valid=$(validate_output "$short_out")
    fi
  fi

  score_penalty="na"
  score_satisfied="na"
  score_total="na"
  if [[ "$SCORE_CONFLICT" -eq 1 && "$conflict_status" -eq 0 ]]; then
    score_line=$(score_output "$conflict_out")
    score_penalty=$(sed -n 's/.*penalty=\([0-9][0-9]*\).*/\1/p' <<<"$score_line")
    score_satisfied=$(sed -n 's/.*satisfied=\([0-9][0-9]*\).*/\1/p' <<<"$score_line")
    score_total=$(sed -n 's/.*total=\([0-9][0-9]*\).*/\1/p' <<<"$score_line")
  fi

  echo "$case_name,$n_val,$family,$arb_status,$arb_time,$arb_len,$short_status,$short_time,$short_len,$conflict_status,$conflict_time,$conflict_len,$conflict_penalty,$arb_valid,$short_valid,$score_penalty,$score_satisfied,$score_total" >> "$OUT"
  echo "benchmarked $case_name (arb ${arb_time}s, short ${short_time}s, conflict ${conflict_time}s)"
  count=$((count + 1))
done

echo "# L=$L max_depth=$MAX_DEPTH timeout=$TIMEOUT limit=$LIMIT" >> "$OUT"
echo "Wrote $OUT"
