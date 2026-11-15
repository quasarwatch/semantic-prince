#!/usr/bin/env bash
# Real-data benchmark + byte-parity for spg targeted vs src/targeted.py.
# data/ is read-only: the reference runs via run_targeted_ref.py (np.savez
# stubbed), and its cold TSV parse is included in the wall time (the C side
# also cold-parses: no .cache_vocab_K10000 npz exists).
set -u

ROOT=$(pwd)/..
C=$ROOT/c
WORK=$C/tests/targeted/work
PYREF=$C/tools/run_targeted_ref.py
SPG=$C/spg

UNI=$ROOT/data/pw_uni.tsv
BI=$ROOT/data/pw_bi.tsv
TEST=$ROOT/data/test.txt

mkdir -p "$WORK"

# 20 personal words for the emit benchmark (top words + a leet form)
head -20 "$ROOT/data/pw_uni.tsv" | cut -f1 > "$WORK/personal20.txt"
printf '1v@n\n' >> "$WORK/personal20.txt"

# f64 dumps (numpy logu/u) for the C parity runs
python3 "$C/tools/dump_targeted_f64.py" "$UNI" "$BI" 10000 \
    "$WORK/real_main.f64" 2>/dev/null
python3 "$C/tools/dump_targeted_f64.py" "$UNI" "$BI" 10000 \
    "$WORK/real_emit.f64" --personal="$WORK/personal20.txt" 2>/dev/null

run_bench() {
  local name=$1; shift
  echo "=== $name ==="
  /usr/bin/time -v python3 "$PYREF" "$@" > "$WORK/$name.py.out" \
      2> "$WORK/$name.py.time"
  local pyrc=$?
  /usr/bin/time -v "$SPG" targeted "$@" --f64dump="$WORK/$name.f64" \
      > "$WORK/$name.c.out" 2> "$WORK/$name.c.time"
  local crc=$?
  echo "rc: py=$pyrc c=$crc"
  if cmp -s "$WORK/$name.py.out" "$WORK/$name.c.out"; then
    echo "stdout: IDENTICAL ($(wc -c < "$WORK/$name.c.out") bytes)"
  else
    echo "stdout: DIFFER"
    diff <(head -20 "$WORK/$name.py.out") <(head -20 "$WORK/$name.c.out") | head -20
  fi
  grep -E "personal words|test slice" "$WORK/$name.py.time" > "$WORK/$name.py.msg" || true
  grep -E "personal words|test slice" "$WORK/$name.c.time" > "$WORK/$name.c.msg" || true
  if cmp -s "$WORK/$name.py.msg" "$WORK/$name.c.msg"; then
    echo "summary(stderr): IDENTICAL: $(cat "$WORK/$name.c.msg")"
  else
    echo "summary(stderr): DIFFER"
    diff "$WORK/$name.py.msg" "$WORK/$name.c.msg" || true
  fi
  echo "python: $(grep -E 'Elapsed|Maximum resident' "$WORK/$name.py.time" | tr '\n' ' ')"
  echo "c:      $(grep -E 'Elapsed|Maximum resident' "$WORK/$name.c.time" | tr '\n' ' ')"
}

run_bench real_main "$UNI" "$BI" "$TEST" 200 10000
run_bench real_emit "$UNI" "$BI" "$TEST" 200 10000 --personal="$WORK/personal20.txt" --emit
