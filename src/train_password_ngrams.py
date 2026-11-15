#!/usr/bin/env python3
# Train word n-grams FROM PASSWORDS, v2b.
# Two-pass:
#   1) count whole alpha runs (len 3-16) in train passwords -> password-folklore vocab
#   2) segment each alpha run by min-cost DP over vocab = formal top-N (len 3-12)
#      UNION top-M frequent runs (len 4-16); costs are normalized log-probs per source,
#      plus per-word boundary penalty BETA; unknown span pays 5/char.
# Counts unigrams (all tokens, incl. len-1 chains) and bigrams (chains len>=2).
# Usage: train_password_ngrams.py train.txt count_1w.txt out_prefix [formal_N] [runs_M]

import sys

def _friendly_excepthook(exc_type, exc, tb):
    if exc_type is FileNotFoundError:
        sys.stderr.write("error: file not found: %s\n" % exc.filename)
        sys.exit(1)
    if exc_type in (ValueError, IndexError):
        sys.stderr.write("error: %s\n" % exc)
        sys.exit(2)
    sys.__excepthook__(exc_type, exc, tb)

sys.excepthook = _friendly_excepthook
import math, re
from collections import Counter

if "--help" in sys.argv or len([a for a in sys.argv[1:] if not a.startswith("--")]) < 1:
    sys.stderr.write("usage: train_password_ngrams.py DUMP.txt   (writes model tables into data/)\n");
    sys.exit(1)
train_path, uni_path, out_prefix = sys.argv[1], sys.argv[2], sys.argv[3]
FORMAL_N = int(sys.argv[4]) if len(sys.argv) > 4 else 50000
RUNS_M = int(sys.argv[5]) if len(sys.argv) > 5 else 30000
BETA = 6.0           # per-word boundary penalty (nats) -> prefer fewer, longer words
UNK_PER_CHAR = 5.0   # unknown span penalty per char

alpha_re = re.compile(r"[a-z]+")

# --- pass 1: count whole alpha runs ---
runs = Counter()
with open(train_path, errors="ignore") as f:
    for line in f:
        for r in alpha_re.findall(line.lower()):
            if 4 <= len(r) <= 16:
                runs[r] += 1
run_total = sum(runs.values())

# --- build vocab: normalized log-probs ---
logp = {}
with open(uni_path) as f:
    formal_total = 0
    tmp = []
    for line in f:
        w, c = line.rstrip("\n").split("\t")
        if w.isalpha() and w.islower() and 3 <= len(w) <= 12:
            c = int(c)
            tmp.append((w, c)); formal_total += c
            if len(tmp) >= FORMAL_N:
                break
    lt = math.log(formal_total)
    for w, c in tmp:
        logp[w] = math.log(c) - lt
lrt = math.log(run_total)
for w, c in runs.most_common(RUNS_M):
    if c < 20:
        break
    lp = math.log(c) - lrt
    if w not in logp or lp > logp[w]:
        logp[w] = lp
print(f"vocab: {len(logp)} (runs seen: {len(runs)})", file=sys.stderr)

_seg_memo = {}
_MISS = object()

def segment(run):
    n = len(run)
    best = [0.0] + [1e18] * n
    par = [-1] * (n + 1)
    for i in range(n):
        if best[i] >= 1e18:
            continue
        bi_ = best[i]
        for j in range(i + 3, min(n, i + 16) + 1):
            w = run[i:j]
            lp = logp.get(w)
            c = bi_ + ((-lp) if lp is not None else UNK_PER_CHAR * (j - i)) + BETA
            if c < best[j]:
                best[j] = c; par[j] = i
    if best[n] >= 1e18:
        return None
    out, j = [], n
    while j > 0:
        i = par[j]
        out.append(run[i:j]); j = i
    out.reverse()
    return out

uni = Counter(); bi = Counter(); nseg = 0
with open(train_path, errors="ignore") as f:
    for line in f:
        pw = line.rstrip("\n").lower()
        parts = alpha_re.findall(pw)
        if not parts:
            continue
        chain = []; ok = True
        for r in parts:
            if len(r) < 3:
                ok = False; break
            if len(r) <= 16 and r in logp:
                chain.append(r)
                continue
            s = _seg_memo.get(r, _MISS)
            if s is _MISS:
                s = segment(r)
                _seg_memo[r] = s
            if s is None:
                ok = False; break
            chain.extend(s)
        if not ok:
            continue
        uni.update(chain)
        if len(chain) >= 2:
            nseg += 1
            bi.update(zip(chain, chain[1:]))

with open(out_prefix + "_uni.tsv", "w") as f:
    for w, c in uni.most_common():
        f.write(f"{w}\t{c}\n")
with open(out_prefix + "_bi.tsv", "w") as f:
    for (a, b), c in bi.most_common():
        f.write(f"{a} {b}\t{c}\n")
print(f"chains>=2: {nseg}, uni={len(uni)}, bi={len(bi)}")
