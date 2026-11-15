#!/usr/bin/env bash
# Byte-parity harness for spg targeted vs src/targeted.py (via
# tools/run_targeted_ref.py, np.savez stubbed). Each case compares
# stdout, stderr and the exit code. The C side gets --f64dump with
# numpy-computed logu/u (tools/dump_targeted_f64.py) to neutralize the
# 1-ulp numpy-SIMD vs libm log/exp difference (plan 5.3 fallback).
set -u

ROOT=$(pwd)/..
C=$ROOT/c
FX=$C/tests/targeted/fixtures
WORK=$C/tests/targeted/work
PYREF=$C/tools/run_targeted_ref.py
DUMP=$C/tools/dump_targeted_f64.py
SPG=$C/spg

mkdir -p "$WORK"

fail=0

run_case() {
  local name=$1; shift
  # args: UNI BI TEST NP K [flags...]
  local extra=()
  for a in "$@"; do
    case "$a" in
      --service=*|--personal=*|--deleet) extra+=("$a");;
    esac
  done
  local dump="$WORK/$name.f64"
  python3 "$DUMP" "$1" "$2" "$5" "$dump" "${extra[@]}" > /dev/null 2>&1 \
    || { echo "FAIL $name: dump"; fail=1; return; }
  python3 "$PYREF" "$@" > "$WORK/$name.py.out" 2> "$WORK/$name.py.err"
  local pyrc=$?
  "$SPG" targeted "$@" --f64dump="$dump" > "$WORK/$name.c.out" 2> "$WORK/$name.c.err"
  local crc=$?
  local ok=1
  cmp -s "$WORK/$name.py.err" "$WORK/$name.c.err" || { ok=0; echo "FAIL $name: stderr"; }
  [ "$pyrc" = "$crc" ] || { ok=0; echo "FAIL $name: rc py=$pyrc c=$crc"; }
  local mode=byte
  if ! cmp -s "$WORK/$name.py.out" "$WORK/$name.c.out"; then
    # emit ordering may diverge inside 1-ulp tie groups (N7: numpy SIMD
    # log/exp vs libm); acceptance per plan 5.1-L2a: identical line set
    if [ -s "$WORK/$name.py.out" ] \
      && cmp -s <(LC_ALL=C sort "$WORK/$name.py.out") \
                <(LC_ALL=C sort "$WORK/$name.c.out"); then
      mode=set
    else
      ok=0
      echo "FAIL $name: stdout"
    fi
  fi
  [ $ok = 1 ] && echo "ok   $name ($mode)" || fail=1
}

U=$FX/uni.tsv;   B=$FX/bi.tsv;     T=$FX/test.txt
UM=$FX/uni_med.tsv; BM=$FX/bi_med.tsv; TM=$FX/test_med.txt

# small model (K=40 > word count -> whole vocab)
run_case small_personal   "$U" "$B" "$T" 200 40 --personal="$FX/personal.txt"
run_case small_service    "$U" "$B" "$T" 200 40 --service=linkedin
run_case small_svc_pers   "$U" "$B" "$T" 200 40 --personal="$FX/personal.txt" --service=linkedin
run_case small_deleet     "$U" "$B" "$T" 200 40 --personal="$FX/personal_leet.txt" --deleet
run_case small_np         "$U" "$B" "$T" 10 40
run_case small_svc_unk    "$U" "$B" "$T" 5 40 --service=gitgram
run_case small_defaults   "$U" "$B" "$T" 200 40

# emit mode
run_case emit_default     "$U" "$B" "$T" 200 40 --personal="$FX/personal.txt" --emit
run_case emit_list        "$U" "$B" "$T" 200 40 "--emit=abc,ivan,ghostword,čćx"
run_case emit_deleet      "$U" "$B" "$T" 200 40 "--emit=1v@n,p@ssw0rd" --deleet
run_case emit_service     "$U" "$B" "$T" 200 40 --service=linkedin --emit
run_case emit_plain       "$U" "$B" "$T" 8 40 --emit

# medium model (K=1200, 50 personal words; 100K checkpoint fires)
run_case med_personal     "$UM" "$BM" "$TM" 200 1200 --personal="$FX/personal50.txt"
run_case med_svc_deleet   "$UM" "$BM" "$TM" 200 1200 --personal="$FX/personal50.txt" --service=myservice --deleet
run_case med_np           "$UM" "$BM" "$TM" 50 1200
run_case med_emit         "$UM" "$BM" "$TM" 200 1200 --personal="$FX/personal50.txt" --emit

exit $fail
