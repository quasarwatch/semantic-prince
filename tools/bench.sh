#!/usr/bin/env bash
# bench.sh -- reproducible benchmark: Semantic PRINCE (campaign v3) vs PRINCE pp64.
#
# Usage:   ./bench.sh <test_set.txt>
# Example: ./bench.sh data/xdump/test_webhost000.txt
#
# Full campaign (defaults, ~20-30 min):
#   OURS = genstruct(K=5000, LIMIT=14M, budget) + genpairs2(pwbi, 25M) + genchains(4M)
#   PP64 = princeprocessor pp64.bin --pw-max=32 < data/words5k.txt, first 100M lines
# Both streams go through src/evaluate.py (hits @ 100K/1M/10M/100M).
#
# Smoke test (a few minutes):
#   SMOKE=1 ./bench.sh <test_set.txt>
# or override individual limits:
#   STRUCT_LIMIT=1000000 PAIRS_LIMIT=5000000 CHAINS_LIMIT=500000 PP_LIMIT=10000000 ./bench.sh ...
#
# NOTE: python scripts using numpy MUST run WITHOUT -I (numpy lives in user-site,
# -I would break the import). evaluate.py runs with -I on purpose.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

TEST="${1:?usage: $0 <test_set.txt>}"
[ -f "$TEST" ] || { echo "test file not found: $TEST" >&2; exit 1; }
TEST="$(readlink -f "$TEST")"

if [ "${SMOKE:-0}" = "1" ]; then
    STRUCT_LIMIT=1000000; PAIRS_LIMIT=5000000; CHAINS_LIMIT=500000; PP_LIMIT=10000000
else
    STRUCT_LIMIT="${STRUCT_LIMIT:-14000000}"
    PAIRS_LIMIT="${PAIRS_LIMIT:-25000000}"
    CHAINS_LIMIT="${CHAINS_LIMIT:-4000000}"
    PP_LIMIT="${PP_LIMIT:-100000000}"
fi
K=5000

WORK=/tmp/sp_bench
mkdir -p "$WORK"

PY=python3
EVAL="$PY -I src/evaluate.py"   # no numpy here: -I is fine and safer

echo "=== bench.sh $(date -u +%Y-%m-%dT%H:%M:%SZ) ==="
echo "test set : $TEST"
echo "limits   : struct=$STRUCT_LIMIT pairs=$PAIRS_LIMIT chains=$CHAINS_LIMIT pp64=$PP_LIMIT"
echo "workdir  : $WORK"
echo

# ---------- OURS: campaign v3 ----------
echo "[1/6] genstruct (budget, K=$K, LIMIT=$STRUCT_LIMIT) ..."
$PY src/genstruct.py data/pw_uni.tsv data/pw_bi.tsv \
    data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv \
    "$K" "$STRUCT_LIMIT" > "$WORK/ours_struct.txt"

echo "[2/6] genpairs2 (pwbi, K=$K, limit=$PAIRS_LIMIT) ..."
$PY src/genpairs2.py data/pw_uni.tsv data/pw_bi.tsv "$K" pwbi "$PAIRS_LIMIT" \
    > "$WORK/ours_pairs.txt"

echo "[3/6] genchains (K=$K, limit=$CHAINS_LIMIT) ..."
$PY src/genchains.py data/pw_uni.tsv data/pw_bi.tsv "$K" "$CHAINS_LIMIT" \
    > "$WORK/ours_chains.txt"

echo "[4/6] evaluate OURS ..."
cat "$WORK/ours_struct.txt" "$WORK/ours_pairs.txt" "$WORK/ours_chains.txt" \
    | $EVAL "$TEST" > "$WORK/ours_eval.txt"
OURS_N=$(cat "$WORK/ours_struct.txt" "$WORK/ours_pairs.txt" "$WORK/ours_chains.txt" | wc -l)

# ---------- PP64 baseline ----------
echo "[5/6] pp64.bin --pw-max=32 < data/words5k.txt (first $PP_LIMIT) ..."
# pp64 dies of SIGPIPE when head closes the pipe -- expected, do not trip pipefail
set +o pipefail
tools/princeprocessor/src/pp64.bin --pw-max=32 < data/words5k.txt \
    | head -n "$PP_LIMIT" > "$WORK/pp64.txt"
set -o pipefail

echo "[6/6] evaluate PP64 ..."
$EVAL "$TEST" "$PP_LIMIT" < "$WORK/pp64.txt" > "$WORK/pp64_eval.txt"

# ---------- table ----------
awk -v ours_n="$OURS_N" -v pp_lim="$PP_LIMIT" '
function hits(file,   line) {
    while ((getline line < file) > 0)
        if (line ~ /guesses ->/ && line !~ /^final:/) {
            gsub(/,/, "", line)
            split(line, a, / *-> */)
            gsub(/[^0-9]/, "", a[1])
            split(a[2], b, / /)
            r[a[1]] = b[1]
        }
    close(file)
}
BEGIN {
    hits("/tmp/sp_bench/ours_eval.txt"); for (k in r) o[k] = r[k]; delete r
    hits("/tmp/sp_bench/pp64_eval.txt"); for (k in r) p[k] = r[k]; delete r
    printf "\n=== RESULTS ===\n"
    printf "%-12s %12s %12s %8s\n", "guesses", "OURS", "PP64", "OURS/PP"
    n = split("100000 1000000 10000000 100000000", cps, " ")
    for (i = 1; i <= n; i++) {
        c = cps[i]
        ov = (c in o) ? o[c] : "-"
        pv = (c in p) ? p[c] : "-"
        ratio = (ov != "-" && pv != "-" && pv+0 > 0) ? sprintf("%.2fx", ov/pv) : "-"
        printf "%-12s %12s %12s %8s\n", c, ov, pv, ratio
    }
    # final lines
    while ((getline line < "/tmp/sp_bench/ours_eval.txt") > 0) if (line ~ /^final:/) { sub(/^final: /, "", line); ofin = line }
    while ((getline line < "/tmp/sp_bench/pp64_eval.txt") > 0) if (line ~ /^final:/) { sub(/^final: /, "", line); pfin = line }
    printf "\nOURS final: %s\nPP64 final: %s\n", ofin, pfin
    printf "\nraw candidate counts: OURS=%s lines, PP64=%s lines\n", ours_n, pp_lim
    printf "full logs: /tmp/sp_bench/ours_eval.txt, /tmp/sp_bench/pp64_eval.txt\n"
}'
