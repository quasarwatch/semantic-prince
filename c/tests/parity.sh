#!/usr/bin/env bash
# parity.sh -- C-vs-Python parity harness for the Semantic PRINCE C port.
#
# For every case: run the Python reference (src/*.py) and the C binary (spg)
# on the same fixture input, then compare:
#   (a) bitwise (cmp)                         -> PASS(bit)
#   (b) on mismatch: canonical comparison     -> PASS(canon)
#       - sets of output lines equal (sort -u | cmp)
#       - hit metrics equal: both streams piped through src/evaluate.py,
#         "final:" lines must match (checkpoint lines may legitimately drift
#         under tie-order permutations; reported as info)
#   (c) rc-parity cases (expected failures)   -> PASS(rc)
#
# Until C components exist, cases SKIP cleanly (missing binary or subcommand)
# and the harness still exits 0. PY_ONLY=1 runs just the Python side against
# the goldens (reference sanity check).
#
# Usage:
#   parity.sh [--level L0|L1] [--case SUBSTR] [-v] [--list]
# Env:
#   SPG            path to C binary        (default: ../spg)
#   PYSRC          path to Python sources  (default: <repo>/src)
#   PY             python3                 (default: python3)
#   SPG_ASSUME_ALL=1  skip subcommand detection (assume all present)
#   PY_ONLY=1      run reference side only
#   SP_FORCE_FULLSORT / SP_FILLER_FULLSORT are exported =1 for the Python
#   side (parity mode per c_port_plan.md 5.2; no-op until patched).
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
C_DIR="$(cd "$HERE/.." && pwd)"
PROJ="$(cd "$C_DIR/.." && pwd)"
PYSRC="${PYSRC:-$PROJ/src}"
SPG="${SPG:-$C_DIR/spg}"
PY="${PY:-python3}"
FX="$HERE/fixtures"
WORK="$HERE/work"
mkdir -p "$WORK"
export SP_FORCE_FULLSORT=1 SP_FILLER_FULLSORT=1
export LC_ALL=C

F_LEVEL=""; F_CASE=""; VERBOSE=0; LIST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --level) F_LEVEL="$2"; shift 2;;
        --case)  F_CASE="$2";  shift 2;;
        -v)      VERBOSE=1; shift;;
        --list)  LIST=1; shift;;
        *) echo "unknown option: $1" >&2; exit 2;;
    esac
done

# --------------------------------------------------------------------------
# case registry
# fields: name|level|subcmd|kind|py_cmd|c_args|opts
#   kind:    stream (default) | eval | selftest
#   opts:    ';'-separated key=value pairs (values may contain spaces):
#     golden=FILE      py stdout must match fixture file bitwise (lines
#                      starting with "emitted:" stripped from the golden)
#     golden_set=FILE  same, but compared as sorted sets
#     evalset=FILE     canonical fallback also compares evaluate.py metrics
#     expect_rc=N      both sides must exit with N (N!=0 -> rc-parity case)
#     expect_lines=N   py stdout line count must be N
#     err_grep=PAT     PAT (multiple separated by ~) must appear in py stderr
#                      and (when C runs) in C stderr
#     stdin=FILE       redirect FILE into both commands
# --------------------------------------------------------------------------
RT="$FX/rt"; RT2="$FX/rt2"; MINI="$FX/mini"
CASES=""
case_add() { CASES="$CASES$1
"; }

# ---- L0: struct (B1, B6, H1, H8) ----
GS_RT="genstruct.py $RT/uni4.tsv $RT/bi4.tsv $RT/tpl.tsv $RT/dig.tsv $RT/sym.tsv"
GS_RT_C="$RT/uni4.tsv $RT/bi4.tsv $RT/tpl.tsv $RT/dig.tsv $RT/sym.tsv"
case_add "struct-rt-budget|L0|struct|stream|$GS_RT 4 24|$GS_RT_C 4 24|golden=$RT/fix_budget.txt"
case_add "struct-rt-global|L0|struct|stream|$GS_RT 4 24 0.7 global|$GS_RT_C 4 24 0.7 global|golden_set=$RT/gs_global.txt"
case_add "struct-b6-flag-mid|L0|struct|stream|$GS_RT 4 10 --no-dedup|$GS_RT_C 4 10 --no-dedup|expect_lines=10"
case_add "struct-b6-flag-pos|L0|struct|stream|$GS_RT 4 10 --no-dedup 0.7 budget 300|$GS_RT_C 4 10 --no-dedup 0.7 budget 300|expect_lines=10"
GS_Z="genstruct.py $RT/uni4.tsv $RT/bi4.tsv $RT2/tplZ.tsv $RT/dig.tsv $RT/sym.tsv"
GS_Z_C="$RT/uni4.tsv $RT/bi4.tsv $RT2/tplZ.tsv $RT/dig.tsv $RT/sym.tsv"
GS_Z2="genstruct.py $RT/uni4.tsv $RT/bi4.tsv $RT2/tplZ2.tsv $RT/dig.tsv $RT/sym.tsv"
GS_Z2_C="$RT/uni4.tsv $RT/bi4.tsv $RT2/tplZ2.tsv $RT/dig.tsv $RT/sym.tsv"
case_add "struct-h1-zero-budget|L0|struct|stream|$GS_Z 4 10|$GS_Z_C 4 10|expect_lines=0"
case_add "struct-h1-zero-global|L0|struct|stream|$GS_Z 4 10 0.7 global|$GS_Z_C 4 10 0.7 global|expect_lines=0"
case_add "struct-h1-zero2-budget|L0|struct|stream|$GS_Z2 4 100|$GS_Z2_C 4 100|expect_lines=12"
case_add "struct-h1-zero2-global|L0|struct|stream|$GS_Z2 4 10 0.7 global|$GS_Z2_C 4 10 0.7 global|expect_lines=10"
case_add "struct-h8-ntpl0|L0|struct|stream|$GS_RT 4 10 0.7 budget 0|$GS_RT_C 4 10 0.7 budget 0|expect_rc=1;err_grep=NTPL (templates cap)"

# ---- L0: pairs (B2) ----
# goldens: rt/fix_pairs* = rt/uni.tsv+bi.tsv;  rt2/p12* = rt2/uni10.tsv+bi10.tsv
GP="genpairs2.py $RT/uni.tsv $RT/bi.tsv"
GP_C="$RT/uni.tsv $RT/bi.tsv"
GP2="genpairs2.py $RT2/uni10.tsv $RT2/bi10.tsv"
GP2_C="$RT2/uni10.tsv $RT2/bi10.tsv"
case_add "pairs-rt-pwbi|L0|pairs|stream|$GP 10 pwbi 12|$GP_C 10 pwbi 12|golden=$RT/fix_pairs.txt;expect_lines=12"
case_add "pairs-rt-pwbi-nodedup|L0|pairs|stream|$GP 10 pwbi 12 --no-dedup|$GP_C 10 pwbi 12 --no-dedup|golden=$RT/fix_pairs_nd.txt;expect_lines=12"
case_add "pairs-rt-freq|L0|pairs|stream|$GP 10 freq 12|$GP_C 10 freq 12|expect_lines=12"
case_add "pairs-rt2-pwbi|L0|pairs|stream|$GP2 10 pwbi 12|$GP2_C 10 pwbi 12|golden=$RT2/p12.txt;expect_lines=12"
case_add "pairs-rt2-pwbi-nodedup|L0|pairs|stream|$GP2 10 pwbi 12 --no-dedup|$GP2_C 10 pwbi 12 --no-dedup|golden=$RT2/p12nd.txt;expect_lines=12"

# ---- L0: chains (B2, B3, H3, H4) ----
# NB: t1/t80 goldens were taken with the default B=200000 (full beam).
# Cases with a cut through a tie group (rt-12/t1/t80, mini) use the
# deterministic reference tools/genchains_ref.py as the Python side: stock
# genchains.py orders ties via np.argpartition, whose order (and therefore
# the cut composition inside boundary tie groups) is build-specific
# (c_port_plan.md N4/R1). Stock-vs-ref differences are confined to boundary
# tie groups with bitwise-equal scores (proof in c/CHAINS.md); ref == C
# bitwise on all these cases. Cases without boundary ties (rt-lam05,
# h4-full) still run against the stock generator.
GCR="$C_DIR/tools/genchains_ref.py $RT/uni.tsv $RT/bi.tsv"
GC="genchains.py $RT/uni.tsv $RT/bi.tsv"
GC_C="$RT/uni.tsv $RT/bi.tsv"
case_add "chains-rt-12|L0|chains|stream|$GCR 10 12|$GC_C 10 12|golden=$RT2/c12_ref.txt;expect_lines=12"
case_add "chains-rt-t1|L0|chains|stream|$GCR 10 12 0.7 200000 1|$GC_C 10 12 0.7 200000 1|golden=$RT2/c12t1_ref.txt;expect_lines=12"
case_add "chains-b3-t80|L0|chains|stream|$GCR 10 12 0.7 200000 80|$GC_C 10 12 0.7 200000 80|golden=$RT2/c12t80_ref.txt;expect_lines=12"
case_add "chains-rt-lam05|L0|chains|stream|$GC 10 12 0.7 12 20 0 127 0.5|$GC_C 10 12 0.7 12 20 0 127 0.5|expect_lines=12"
case_add "chains-h4-full|L0|chains|stream|$GC 10 1000|$GC_C 10 1000|golden=$RT2/cfull_fixed.txt;expect_lines=694"
case_add "chains-h3-b0|L0|chains|stream|$GC 10 12 0.7 0|$GC_C 10 12 0.7 0|expect_rc=1;err_grep=B (beam width)"
case_add "chains-h3-t0|L0|chains|stream|$GC 10 12 0.7 12 0|$GC_C 10 12 0.7 12 0|expect_rc=1;err_grep=T (continuations"
case_add "chains-h3-lam2|L0|chains|stream|$GC 10 12 0.7 12 20 0 127 2.0|$GC_C 10 12 0.7 12 20 0 127 2.0|expect_rc=1;err_grep=lam must be"

# ---- L0: targeted (B4, B5, H9, H6) ----
TG="targeted.py $RT/uni.tsv $RT/bi.tsv $RT/test_b4.txt 50 10"
TG_C="$RT/uni.tsv $RT/bi.tsv $RT/test_b4.txt 50 10"
case_add "targeted-b4|L0|targeted|stream|$TG --personal=$RT/personal.txt|$TG_C --personal=$RT/personal.txt|"
case_add "targeted-b5-service|L0|targeted|stream|$TG --personal=$RT/personal.txt --service=linkedin|$TG_C --personal=$RT/personal.txt --service=linkedin|err_grep=personal words: 6"
case_add "targeted-h9-dup|L0|targeted|stream|targeted.py $RT/uni.tsv $RT/bi.tsv $RT/test_b4.txt 50 10 --personal=$RT2/pers_dup.txt|$TG_C --personal=$RT2/pers_dup.txt|err_grep=3 words -> 2~personal words: 2"
case_add "targeted-h6-dup|L0|targeted|stream|targeted.py $RT/uni.tsv $RT/bi.tsv $RT2/test_dup.txt 50 10 --personal=$RT/personal.txt|$RT/uni.tsv $RT/bi.tsv $RT2/test_dup.txt 50 10 --personal=$RT/personal.txt|err_grep=test slice: 2"
case_add "targeted-emit|L0|targeted|stream|$TG --emit=ab,xx|$TG_C --emit=ab,xx|"

# ---- L0: evaluate (H7) ----
case_add "eval-crlf-set|L0|evaluate|eval|-I evaluate.py $RT2/crlf_test.txt|$RT2/crlf_test.txt|stdin=$RT2/stream_abcd.txt"
case_add "eval-crlf-stream|L0|evaluate|eval|-I evaluate.py $RT/test1.txt|$RT/test1.txt|stdin=$RT2/stream_crlf.txt"
case_add "eval-mini|L0|evaluate|eval|-I evaluate.py $MINI/test.txt|$MINI/test.txt|stdin=$MINI/stream.txt"

# ---- L1: mini dictionary (30 words) ----
GSM="genstruct.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv $MINI/tpl.tsv $MINI/dig.tsv $MINI/sym.tsv"
GSM_C="$MINI/pw_uni.tsv $MINI/pw_bi.tsv $MINI/tpl.tsv $MINI/dig.tsv $MINI/sym.tsv"
case_add "struct-mini-budget|L1|struct|stream|$GSM 30 2000|$GSM_C 30 2000|evalset=$MINI/test.txt"
case_add "struct-mini-global|L1|struct|stream|$GSM 30 2000 0.7 global|$GSM_C 30 2000 0.7 global|evalset=$MINI/test.txt"
case_add "pairs-mini-pwbi|L1|pairs|stream|genpairs2.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 pwbi 5000|$MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 pwbi 5000|evalset=$MINI/test.txt"
case_add "pairs-mini-freq|L1|pairs|stream|genpairs2.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 freq 5000|$MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 freq 5000|evalset=$MINI/test.txt"
case_add "chains-mini|L1|chains|stream|$C_DIR/tools/genchains_ref.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 5000 0.7 2000 10|$MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 5000 0.7 2000 10|evalset=$MINI/test.txt"
case_add "chains-mini-lam|L1|chains|stream|$C_DIR/tools/genchains_ref.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 5000 0.7 2000 10 0 127 0.3|$MINI/pw_uni.tsv $MINI/pw_bi.tsv 30 5000 0.7 2000 10 0 127 0.3|evalset=$MINI/test.txt"
case_add "targeted-mini-emit|L1|targeted|stream|targeted.py $MINI/pw_uni.tsv $MINI/pw_bi.tsv $MINI/test.txt 2 30 --personal=$MINI/personal.txt --emit=love|$MINI/pw_uni.tsv $MINI/pw_bi.tsv $MINI/test.txt 2 30 --personal=$MINI/personal.txt --emit=love|"

# ---- L0: model dump (gate G1: array checksums; py ref is tools/dump_ref.py)
DUMPREF="$C_DIR/tools/dump_ref.py"
[ -f "$DUMPREF" ] && {
    case_add "dump-rt|L0|dump|stream|$DUMPREF $RT/uni.tsv $RT/bi.tsv 10|$RT/uni.tsv $RT/bi.tsv 10|"
    case_add "dump-rt2|L0|dump|stream|$DUMPREF $RT2/uni10.tsv $RT2/bi10.tsv 10|$RT2/uni10.tsv $RT2/bi10.tsv 10|"
    case_add "dump-mini|L1|dump|stream|$DUMPREF $MINI/pw_uni.tsv $MINI/pw_bi.tsv 30|$MINI/pw_uni.tsv $MINI/pw_bi.tsv 30|"
}

# ---- selftest (C-only, covers B1..B7 on built-in fixtures) ----
case_add "spg-selftest|L0|selftest|selftest|||"

# --------------------------------------------------------------------------
LOG="$WORK/parity.log"
: > "$LOG"
log() { echo "$*" | tee -a "$LOG"; }

# environment header (plan 5.4: versions + input hashes)
{
    echo "== parity run $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "python : $("$PY" --version 2>&1) ($PY)"
    "$PY" -c "import numpy; print('numpy  :', numpy.__version__)" 2>/dev/null || echo "numpy  : n/a"
    echo "cc     : $(cc --version 2>/dev/null | head -1 || echo n/a)"
    echo "spg    : $SPG"
    echo "pysrc  : $PYSRC"
    echo "SP_FORCE_FULLSORT=$SP_FORCE_FULLSORT SP_FILLER_FULLSORT=$SP_FILLER_FULLSORT"
    echo "input sha256:"
    find "$FX" \( -name '*.tsv' -o -name '*.txt' \) | sort | xargs sha256sum | sed 's/^/  /'
    echo
} >> "$LOG"

# subcommand detection -------------------------------------------------------
SPG_USAGE=""
spg_ok=0
if [ -x "$SPG" ]; then
    SPG_USAGE="$("$SPG" 2>&1 || true)"
    spg_ok=1
fi
have_subcmd() {
    [ "$spg_ok" = 1 ] || return 1
    [ "${SPG_ASSUME_ALL:-0}" = "1" ] && return 0
    echo "$SPG_USAGE" | grep -qw "$1" || return 1
    # a usage line marked "(stub)" means present-but-not-implemented -> skip
    if echo "$SPG_USAGE" | grep -E "^[[:space:]]*$1[[:space:]]" | grep -q "(stub)"; then
        return 1
    fi
    return 0
}

# option parsing (';'-separated so values may contain spaces) ----------------
opt_get() { # opts key -> value or empty
    local oldifs="$IFS" o; IFS=';'
    for o in $1; do
        case "$o" in
            "$2="*) IFS="$oldifs"; echo "${o#"$2="}"; return;;
        esac
    done
    IFS="$oldifs"
}

# evaluate helper: run python evaluate over a stream file
eval_metrics() { # stream_file testset
    ( cd "$PYSRC" && "$PY" -I evaluate.py "$2" 100000000 < "$1" ) 2>/dev/null
}

N_PASS_BIT=0; N_PASS_CANON=0; N_PASS_RC=0; N_FAIL=0; N_SKIP=0; N_PASS_PY=0

run_case() {
    local row="$1"
    local name level subcmd kind pycmd cargs opts
    local oldifs="$IFS"; IFS='|'
    read -r name level subcmd kind pycmd cargs opts <<< "$row"
    IFS="$oldifs"

    [ -n "$F_LEVEL" ] && [ "$level" != "$F_LEVEL" ] && return
    [ -n "$F_CASE" ] && [[ "$name" != *"$F_CASE"* ]] && return
    if [ "$LIST" = 1 ]; then echo "$level $name"; return; fi

    local golden evalset expect_rc expect_lines err_grep stdin
    golden="$(opt_get "$opts" golden)"
    [ -z "$golden" ] && golden="$(opt_get "$opts" golden_set)"
    evalset="$(opt_get "$opts" evalset)"
    expect_rc="$(opt_get "$opts" expect_rc)"
    expect_lines="$(opt_get "$opts" expect_lines)"
    err_grep="$(opt_get "$opts" err_grep)"
    stdin="$(opt_get "$opts" stdin)"

    local b="$WORK/$name"

    # --- run python reference ---
    local rc_py=0
    if [ -n "$pycmd" ]; then
        if [ -n "$stdin" ]; then
            ( cd "$PYSRC" && "$PY" $pycmd < "$stdin" ) > "$b.py.out" 2> "$b.py.err"
        else
            ( cd "$PYSRC" && "$PY" $pycmd ) > "$b.py.out" 2> "$b.py.err"
        fi
        rc_py=$?
        echo "$rc_py" > "$b.py.rc"
    fi

    # --- reference-side assertions (goldens, rc, line counts, stderr) ---
    local ref_fail=""
    if [ -n "$expect_rc" ] && [ "$rc_py" != "$expect_rc" ]; then
        ref_fail="py rc=$rc_py, expected $expect_rc"
    fi
    if [ -z "$ref_fail" ] && [ -n "$expect_lines" ]; then
        local nl; nl=$(wc -l < "$b.py.out")
        [ "$nl" = "$expect_lines" ] || ref_fail="py lines=$nl, expected $expect_lines"
    fi
    if [ -z "$ref_fail" ] && [ -n "$err_grep" ]; then
        local pat; local o2="$IFS"; IFS='~'
        for pat in $err_grep; do
            grep -qF "$pat" "$b.py.err" || ref_fail="py stderr lacks: $pat"
        done
        IFS="$o2"
    fi
    if [ -z "$ref_fail" ] && [ -n "$golden" ]; then
        case "$opts" in
            *golden_set=*)
                grep -v '^emitted:' "$golden" | LC_ALL=C sort -u > "$b.golden.s"
                LC_ALL=C sort -u "$b.py.out" | cmp -s - "$b.golden.s" \
                    || ref_fail="py output drifted from golden(set) $golden"
                ;;
            *)
                grep -v '^emitted:' "$golden" | cmp -s - "$b.py.out" \
                    || ref_fail="py output drifted from golden $golden"
                ;;
        esac
    fi
    if [ -n "$ref_fail" ]; then
        log "FAIL    $level $name -- REFERENCE: $ref_fail"
        N_FAIL=$((N_FAIL+1))
        [ "$VERBOSE" = 1 ] && tail -5 "$b.py.err" | sed 's/^/    py.err: /' | tee -a "$LOG"
        return
    fi

    # --- C side availability ---
    if [ "${PY_ONLY:-0}" = "1" ]; then
        log "PASS-PY $level $name (reference ok; C side skipped by PY_ONLY)"
        N_PASS_PY=$((N_PASS_PY+1)); return
    fi
    if [ "$kind" = "selftest" ]; then
        if ! have_subcmd selftest; then
            log "SKIP    $level $name (no spg subcommand 'selftest')"
            N_SKIP=$((N_SKIP+1)); return
        fi
        "$SPG" selftest > "$b.c.out" 2> "$b.c.err"; local rc=$?
        if [ "$rc" = 0 ]; then
            log "PASS(bit) $level $name (selftest rc=0)"; N_PASS_BIT=$((N_PASS_BIT+1))
        else
            log "FAIL    $level $name (selftest rc=$rc)"; N_FAIL=$((N_FAIL+1))
        fi
        return
    fi
    if ! have_subcmd "$subcmd"; then
        log "SKIP    $level $name (no spg subcommand '$subcmd')"
        N_SKIP=$((N_SKIP+1)); return
    fi

    # --- run C ---
    local rc_c=0
    if [ -n "$stdin" ]; then
        "$SPG" "$subcmd" $cargs < "$stdin" > "$b.c.out" 2> "$b.c.err"
    else
        "$SPG" "$subcmd" $cargs > "$b.c.out" 2> "$b.c.err"
    fi
    rc_c=$?
    echo "$rc_c" > "$b.c.rc"

    # --- rc parity ---
    if [ "$rc_py" != "$rc_c" ]; then
        log "FAIL    $level $name (rc: py=$rc_py c=$rc_c)"
        N_FAIL=$((N_FAIL+1)); return
    fi
    if [ -n "$err_grep" ]; then
        local o3="$IFS"; IFS='~'; local pat okc=1
        for pat in $err_grep; do grep -qF "$pat" "$b.c.err" || okc=0; done
        IFS="$o3"
        if [ "$okc" = 0 ]; then
            log "FAIL    $level $name (c stderr lacks: $err_grep)"
            N_FAIL=$((N_FAIL+1)); return
        fi
    fi
    if [ "${expect_rc:-0}" != "0" ]; then
        log "PASS(rc) $level $name (both rc=$rc_py)"
        N_PASS_RC=$((N_PASS_RC+1)); return
    fi

    # --- bitwise ---
    if cmp -s "$b.py.out" "$b.c.out"; then
        log "PASS(bit) $level $name"
        N_PASS_BIT=$((N_PASS_BIT+1)); return
    fi

    # --- canonical fallback ---
    LC_ALL=C sort -u "$b.py.out" > "$b.py.set"
    LC_ALL=C sort -u "$b.c.out"  > "$b.c.set"
    local sets_ok=0 met_ok=1 met_note=""
    cmp -s "$b.py.set" "$b.c.set" && sets_ok=1
    if [ "$sets_ok" = 1 ] && [ -n "$evalset" ]; then
        eval_metrics "$b.py.out" "$evalset" > "$b.py.eval"
        eval_metrics "$b.c.out"  "$evalset" > "$b.c.eval"
        local fp fc
        fp=$(grep '^final:' "$b.py.eval"); fc=$(grep '^final:' "$b.c.eval")
        if [ "$fp" != "$fc" ]; then
            met_ok=0; met_note="metrics differ: py[$fp] c[$fc]"
        elif ! cmp -s "$b.py.eval" "$b.c.eval"; then
            met_note="checkpoint drift (final equal)"
        fi
    fi
    local off; off=$(cmp "$b.py.out" "$b.c.out" 2>&1 | head -1)
    if [ "$sets_ok" = 1 ] && [ "$met_ok" = 1 ]; then
        log "PASS(canon) $level $name (sets equal; first byte diff: ${off:-none}${met_note:+; $met_note})"
        N_PASS_CANON=$((N_PASS_CANON+1))
    else
        local np nc; np=$(wc -l < "$b.py.out"); nc=$(wc -l < "$b.c.out")
        log "FAIL    $level $name (lines py=$np c=$nc; sets_equal=$sets_ok${met_note:+; $met_note}; first diff: ${off:-none})"
        N_FAIL=$((N_FAIL+1))
    fi
}

while IFS= read -r row; do
    [ -n "$row" ] && run_case "$row"
done <<< "$CASES"

if [ "$LIST" != 1 ]; then
    log ""
    log "== summary: PASS(bit)=$N_PASS_BIT PASS(canon)=$N_PASS_CANON PASS(rc)=$N_PASS_RC PASS-PY=$N_PASS_PY SKIP=$N_SKIP FAIL=$N_FAIL"
fi
[ "$N_FAIL" = 0 ]
