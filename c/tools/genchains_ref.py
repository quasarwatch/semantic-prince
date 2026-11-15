#!/usr/bin/env python3
# Deterministic parity reference for c/src/chains.c ("spg chains").
#
# This is src/genchains.py (post bugfixes_v1/v2) with exactly three changes,
# each replacing an np.argpartition call (whose output order is unspecified,
# see c_port_plan.md N4) by a full stable sort with a fixed canonical order:
#
#   1. seam selection:  top = argsort(-flat, stable)[:Bb]
#      -> seams ordered (score desc, flat idx asc)
#   2. per-row top-T:   t = argsort(-blk, axis=1, stable)[:, :T]
#      -> continuations ordered (score desc, column asc)
#   3. global cut:      always the full-sort branch
#      order = argsort(sc, stable)[::-1][:cut]
#      -> (score desc, candidate idx desc); this is the code path the
#      original takes whenever cut >= sc.size // 4
#
# Everything else (BS blocking, masks, dedup, top-up, chunking, messages)
# is byte-identical to genchains.py. np.savez is patched out so a cache
# miss never writes into data/.
import sys, os

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "src"))

import numpy as np

np.savez = lambda *a, **k: None  # keep data/ read-only

from spmodel import load_vocab_model

uni_path, bi_path, K, limit = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
ALPHA = float(sys.argv[5]) if len(sys.argv) > 5 else 0.7
B = int(sys.argv[6]) if len(sys.argv) > 6 else 200_000
T = int(sys.argv[7]) if len(sys.argv) > 7 else 20
LMIN = int(sys.argv[8]) if len(sys.argv) > 8 else 0
LMAX = int(sys.argv[9]) if len(sys.argv) > 9 else 127
LAM = float(sys.argv[10]) if len(sys.argv) > 10 else 0.0

if B <= 0:
    sys.exit("error: B (beam width) must be >= 1, got %d" % B)
if T <= 0:
    sys.exit("error: T (continuations per seam) must be >= 1, got %d" % T)
if not 0.0 <= LAM <= 1.0:
    sys.exit("error: lam must be in [0, 1], got %s" % LAM)

m = load_vocab_model(uni_path, bi_path, K)
words, wb, counts, logu = m.words, m.wb, m.counts, m.logu
K = len(words)
T = min(T, K)  # argpartition kth would go out of bounds for T > K

M = np.zeros((K, K))
M[m.bi_i, m.bi_j] = m.bi_cnt
cf = counts.astype(np.float64)
pjm = ALPHA * (M / cf[:, None]) + (1 - ALPHA) * (cf / cf.sum())[None, :]
del M
logp = np.log(np.maximum(pjm, 1e-15))
del pjm

S2 = logu[:, None] + logp          # first-seam scores
np.fill_diagonal(S2, -np.inf)

# top-B first seams  [PATCH 1: canonical order (score desc, flat idx asc)]
flat = S2.ravel()
Bb = min(B, flat.size)
top = np.argsort(-flat, kind="stable")[:Bb]
i1, j1 = np.unravel_index(top, (K, K))
s1 = flat[top]
del flat, S2, top

# extend each seam by top-T continuations from row j1
budget_bytes = 600_000_000 if LAM > 0.0 else 1_200_000_000
BS = max(5_000, min(50_000, budget_bytes // (K * 8)))
cand_s, cand_i, cand_j, cand_k = [], [], [], []
for s in range(0, len(i1), BS):
    e = min(s + BS, len(i1))
    blk = logp[j1[s:e]]            # (b, K)
    if LAM > 0.0:
        bi_ = logp[i1[s:e]]        # (b, K) second parent context
        mx = np.maximum(blk, bi_)
        # log( lam*exp(bj) + (1-lam)*exp(bi) ), stabilized
        blk = mx + np.log(LAM * np.exp(blk - mx) + (1.0 - LAM) * np.exp(bi_ - mx))
        del bi_, mx
    # [PATCH 2: canonical per-row order (score desc, column asc)]
    t = np.argsort(-blk, axis=1, kind="stable")[:, :T]              # (b, T)
    ts = np.take_along_axis(blk, t, axis=1)             # (b, T)
    b_idx = np.repeat(np.arange(s, e), T)
    cand_s.append(s1[s:e].repeat(T) + ts.ravel())
    cand_i.append(i1[s:e].repeat(T))
    cand_j.append(j1[s:e].repeat(T))
    cand_k.append(t.ravel())
    del blk, t, ts

sc = np.concatenate(cand_s)
ii = np.concatenate(cand_i)
jj = np.concatenate(cand_j)
kk = np.concatenate(cand_k)
del cand_s, cand_i, cand_j, cand_k, logp

ok = kk != ii  # w3 != w1 (w3 == w2 row was not masked; mask now)
ok &= kk != jj
if LMIN > 0 or LMAX < 127:  # length band on the concatenated chain
    wl = np.array([len(w) for w in wb], dtype=np.int32)
    clen = wl[ii] + wl[jj] + wl[kk]
    ok &= (clen >= LMIN) & (clen <= LMAX)
    del clen, wl
mask_diag = ~(ok)
sc[mask_diag] = -np.inf

# global top-limit (dedup before the budget is spent: top up the shortfall)
wba = np.array(wb, dtype=object)
out = sys.stdout.buffer
seen = set()
CH = 500_000
emitted = 0
start = 0                    # sorted candidates already scanned
cut = min(limit, sc.size)    # selection depth; topped up on dedup shortfall
while start < sc.size:
    # [PATCH 3: always the full-sort branch -> (score desc, idx desc)]
    order = np.argsort(sc, kind="stable")[::-1][:cut]
    dry = False
    for s in range(start, cut, CH):
        e = min(s + CH, cut)
        o = order[s:e]
        ninf = np.isneginf(sc[o])
        if ninf.any():  # masked region reached (chunk interior too): space over
            dry = True
            o = o[:ninf.argmax()]
        parts = (wba[ii[o]] + wba[jj[o]] + wba[kk[o]]).tolist()
        parts = [p for p in parts if not (p in seen or seen.add(p))]
        parts = parts[:limit - emitted]
        if parts:
            out.write(b"\n".join(parts) + b"\n")
            emitted += len(parts)
        if dry:
            break
    del order
    start = cut
    if dry or emitted >= limit or cut == sc.size:
        break
    cut = min(cut + 2 * (limit - emitted), sc.size)  # recover deduped slots
out.flush()
print("emitted: %d" % emitted, file=sys.stderr)
