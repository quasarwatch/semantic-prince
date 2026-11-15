#!/usr/bin/env python3
# Generator v2: pair chains over password-derived stats. Same pair space, 3 orderings:
#   freq  - log u1 + log u2 (password unigram product)
#   pwbi  - log u1 + log P_jm(w2|w1), Jelinek-Mercer: a*C(w1,w2)/C(w1) + (1-a)*u(w2)
# Emits w1w2, best first, w1 != w2, duplicate strings removed (different splits
# can glue to the same candidate).
# Usage: genpairs2.py pw_uni.tsv pw_bi.tsv K mode limit [alpha] [--no-dedup]
import sys
import os
import numpy as np
from spmodel import load_vocab_model

# parity mode for the C port (c_port_plan.md 5.2): always take the full
# stable argsort branch (deterministic on ties); default behavior unchanged
FULLSORT = os.environ.get("SP_FORCE_FULLSORT") == "1"

uni_path, bi_path, K, mode, limit = sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], int(sys.argv[5])
ALPHA = float(sys.argv[6]) if len(sys.argv) > 6 and sys.argv[6] != "--no-dedup" else 0.7
DEDUP = "--no-dedup" not in sys.argv

m = load_vocab_model(uni_path, bi_path, K)
words, wb, counts, logu = m.words, m.wb, m.counts, m.logu
K = len(words)
space = K * K - K  # non-diagonal pairs (diagonal scores -inf, never selected)
limit = min(limit, space)

if mode == "pwbi":
    M = np.zeros((K, K))
    M[m.bi_i, m.bi_j] = m.bi_cnt
    cf = counts.astype(np.float64)
    pcond = M / cf[:, None]                       # C(w2|w1) MLE
    del M
    pjm = ALPHA * pcond + (1 - ALPHA) * (cf / cf.sum())[None, :]
    del pcond
    logp = np.log(np.maximum(pjm, 1e-15))
    del pjm
    S = logu[:, None] + logp
    del logp
else:
    S = logu[:, None] + logu[None, :]
np.fill_diagonal(S, -np.inf)

flat = S.ravel()
del S
wba = np.array(wb, dtype=object)
out = sys.stdout.buffer
seen = set() if DEDUP else None
CH = 1_000_000
emitted = 0
start = 0                    # sorted candidates already scanned
cut = limit                  # selection depth; topped up on dedup shortfall
while start < space:
    if cut < flat.size // 4 and not FULLSORT:
        top = np.argpartition(flat, -cut)[-cut:]
        order = top[np.argsort(flat[top], kind="stable")[::-1]]
        del top
    else:
        order = np.argsort(flat, kind="stable")[::-1][:cut]
    i_all, j_all = np.unravel_index(order[start:], (K, K))
    del order
    for s in range(0, len(i_all), CH):
        e = min(s + CH, len(i_all))
        parts = (wba[i_all[s:e]] + wba[j_all[s:e]]).tolist()
        if DEDUP:
            parts = [p for p in parts if not (p in seen or seen.add(p))]
        parts = parts[:limit - emitted]
        if parts:
            out.write(b"\n".join(parts) + b"\n")
            emitted += len(parts)
    del i_all, j_all
    start = cut
    if emitted >= limit or cut == space:
        break
    cut = min(cut + 2 * (limit - emitted), space)  # recover deduped slots
del flat
out.flush()
print("emitted: %d" % emitted, file=sys.stderr)
