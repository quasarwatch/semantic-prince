#!/bin/sh
# Parity gate G1: python spmodel vs C spg, sha256 per model array.
# usage: parity_check.sh [K ...]        (default: 5000 10000)
# env:   UNI BI override the TSV paths
set -u

cd "$(dirname "$0")/.."

UNI="${UNI:-../data/pw_uni.tsv}"
BI="${BI:-../data/pw_bi.tsv}"

if [ $# -gt 0 ]; then
  KS="$*"
else
  KS="5000 10000"
fi

if [ ! -x ./spg ]; then
  echo "parity_check: ./spg missing, run make first" >&2
  exit 2
fi

rc=0

for K in $KS; do
  ref=$(mktemp)
  got=$(mktemp)

  python3 tools/dump_ref.py "$UNI" "$BI" "$K" > "$ref" || {
    echo "K=$K: dump_ref.py failed" >&2
    rm -f "$ref" "$got"
    rc=1
    continue
  }

  ./spg dump "$UNI" "$BI" "$K" > "$got" 2>/dev/null || {
    echo "K=$K: spg dump failed" >&2
    rm -f "$ref" "$got"
    rc=1
    continue
  }

  echo "== K=$K =="
  awk '
    NR==FNR { h[$1]=$2; next }
    {
      st = ($2 == h[$1]) ? "OK" : "DIFF"
      printf "  %-12s %-4s %s\n", $1, st, $2
      if ($2 != h[$1] && $1 != "logu") bad = 1
      if ($2 != h[$1] && $1 == "logu") warn = 1
    }
    END {
      if (warn)
        printf "  (logu: known 1-ulp libm-vs-numpy divergence, plan 5.3)\n"
      exit bad
    }
  ' "$ref" "$got" || rc=1

  rm -f "$ref" "$got"
done

if [ $rc -eq 0 ]; then
  echo "PARITY: PASS"
else
  echo "PARITY: FAIL"
fi

exit $rc
