#!/usr/bin/env python3
# Struct-mode generator v2: fill skeleton templates, most probable first.
#   A-run -> single words or word pairs (bigram seam) of that total length
#   D-run -> digit values of that length by train frequency
#   S-run -> symbol strings of that length by train frequency
# Modes:
#   budget (default): templates in train-frequency order, per-template budget
#     proportional to its share -- empirically beats global at 100K/10M,
#     because digit/symbol segments have lower entropy and their conditional
#     scores are not calibrated against word segments
#   global: lazy best-first per template merged by joint score
#     (content score + log template prior) -- wins only around ~1M guesses
#     (measured 2026-11-12: 4219/21561/52373 vs budget 7089/16710/64725)
# Per-template stream: exact best-first heap; for small grids (prod <= GRID_MAX)
# a fully sorted numpy grid instead (same order up to ~1-ulp score ties, faster).
# Fillers are cached in data/.cache_fillers_*.pkl (keyed by source sig).
# Usage:
#   genstruct.py uni.tsv bi.tsv templates.tsv digits.tsv syms.tsv K LIMIT
#                [alpha] [mode] [ntemplates] [--no-dedup]
import sys, os, math, heapq, re, pickle, json
import numpy as np
from spmodel import load_vocab_model, _sig, _DATA

pos = [a for a in sys.argv[1:] if a != "--no-dedup"]
(uni_path, bi_path, tpl_path, dig_path, sym_path) = pos[0:5]
K, LIMIT = int(pos[5]), int(pos[6])
ALPHA = float(pos[7]) if len(pos) > 7 else 0.7
MODE = pos[8] if len(pos) > 8 else "budget"
NTPL = int(pos[9]) if len(pos) > 9 else 300
DEDUP = "--no-dedup" not in sys.argv
GAMMA = float(os.environ.get("GENSTRUCT_GAMMA", "1.0"))  # D/S score penalty (global mode calibration)
GRID_MAX = 2_000_000
MAXL = 16
TOP_SRC, TOP_FILL = 400, 600
# SP_FILLER_FULLSORT=1: deterministic filler construction for C-parity runs
# (c_port_plan.md 5.2): stable argsort for bylen and a full stable sort of
# each pair sublattice instead of argpartition (whose selection order is
# build-dependent). The pickle cache holds argpartition-ordered fillers, so
# in this mode it is neither read nor written.
_FULLSORT = os.environ.get("SP_FILLER_FULLSORT") == "1"

# --- word model (cached) ---
m = load_vocab_model(uni_path, bi_path, K)
words, wb, counts, logu = m.words, m.wb, m.counts, m.logu
K = len(words)
wlen = np.array([len(w) for w in words], dtype=np.int8)

# --- alpha fillers per length: singles + pairs, deduped, top TOP_FILL ---
fcache = os.path.join(_DATA, ".cache_fillers_K%d_a%s.pkl" % (K, ALPHA))
fsig = json.dumps(_sig([uni_path, bi_path]) + [TOP_SRC, TOP_FILL])
fillers = None
if os.path.exists(fcache) and not _FULLSORT:
    try:
        with open(fcache, "rb") as f:
            sig, fillers = pickle.load(f)
        if sig != fsig:
            fillers = None
    except Exception:
        fillers = None

if fillers is None:
    cf = counts.astype(np.float64)
    M = np.zeros((K, K))
    M[m.bi_i, m.bi_j] = m.bi_cnt
    pjm = ALPHA * (M / cf[:, None]) + (1 - ALPHA) * (cf / cf.sum())[None, :]
    del M
    PS = logu[:, None] + np.log(np.maximum(pjm, 1e-15))
    del pjm
    bylen = {}
    for l in range(2, 11):
        idx = np.where(wlen == l)[0]
        if _FULLSORT:
            bylen[l] = idx[np.argsort(counts[idx], kind="stable")[::-1][:TOP_SRC]]
        else:
            bylen[l] = idx[np.argsort(counts[idx])[::-1][:TOP_SRC]]
    fmap = {}  # L -> {bytes: score}
    # singles (plain loop, K is small)
    for i, w in enumerate(wb):
        fmap.setdefault(int(wlen[i]), {})[w] = float(logu[i])
    # pairs
    for l1 in range(2, 11):
        i1 = bylen[l1]
        for l2 in range(2, 11):
            L = l1 + l2
            if L > MAXL:
                continue
            i2 = bylen[l2]
            sub = PS[np.ix_(i1, i2)]
            flat = sub.ravel()
            keep = min(TOP_FILL * 4, flat.size)
            if _FULLSORT:
                top = np.argsort(-flat, kind="stable")[:keep]
            else:
                top = np.argpartition(flat, -keep)[-keep:]
            a, b = np.unravel_index(top, sub.shape)
            d = fmap.setdefault(L, {})
            for x, y in zip(a.tolist(), b.tolist()):
                if i1[x] == i2[y]:
                    continue
                s = float(sub[x, y])
                key = wb[i1[x]] + wb[i2[y]]
                if key not in d or s > d[key]:
                    d[key] = s
    del PS
    fillers = {}
    for L, d in fmap.items():
        items = sorted(d.items(), key=lambda kv: -kv[1])[:TOP_FILL]
        fillers[L] = ([k for k, _ in items], np.array([v for _, v in items]))
    if not _FULLSORT:
        with open(fcache, "wb") as f:
            pickle.dump((fsig, fillers), f, protocol=4)

# --- digits and symbols per length ---
def load_run_values(path):
    vals, totals = {}, {}
    with open(path) as f:
        for line in f:
            l, v, c = line.rstrip("\n").split("\t")
            l = int(l)
            c = int(c)
            vals.setdefault(l, []).append((v.encode(), c))
            totals[l] = totals.get(l, 0) + c
    out = {}
    for l, lst in vals.items():
        t = totals[l]
        out[l] = ([v for v, _ in lst],
                  np.array([math.log(c / t) * GAMMA for _, c in lst]))
    return out

dig = load_run_values(dig_path)
sym = load_run_values(sym_path)

# --- templates ---
seg_re = re.compile(r"([ADS])(\d+)")
tpls = []
with open(tpl_path) as f:
    for line in f:
        t, c = line.rstrip("\n").split("\t")
        segs = []
        ok = True
        for cls, ln in seg_re.findall(t):
            src = fillers if cls == "A" else (dig if cls == "D" else sym)
            lst = src.get(int(ln))
            if not lst:
                ok = False
                break
            segs.append(lst)
        if ok:
            tpls.append((int(c), segs))
        if len(tpls) == NTPL:
            break


class HeapGen:
    """Lazy exact best-first k-way merge within one template."""
    __slots__ = ("segs", "heap", "seen")

    def __init__(self, segs):
        self.segs = segs
        s0 = sum(float(seg[1][0]) for seg in segs)
        self.heap = [(-s0, (0,) * len(segs))]
        self.seen = {(0,) * len(segs)}

    def __iter__(self):
        return self

    def __next__(self):
        while self.heap:
            negscore, idx = heapq.heappop(self.heap)
            segs = self.segs
            cand = b"".join(segs[k][0][idx[k]] for k in range(len(segs)))
            for k in range(len(segs)):
                nk = idx[k] + 1
                if nk < len(segs[k][0]):
                    ni = list(idx)
                    ni[k] = nk
                    ni = tuple(ni)
                    if ni not in self.seen:
                        self.seen.add(ni)
                        s = -negscore - segs[k][1][idx[k]] + segs[k][1][nk]
                        heapq.heappush(self.heap, (-s, ni))
            return (-negscore, cand)
        raise StopIteration


class GridGen:
    """Fully sorted grid for small templates; same order as HeapGen except at
    exact score ties: the heap accumulates scores incrementally while the grid
    broadcasts the sum, so tied scores can differ by ~1 ulp and permute
    (harmless apart from checkpoint boundaries)."""
    __slots__ = ("segs", "order", "flat_scores", "pos", "shape")

    def __init__(self, segs):
        self.segs = segs
        self.pos = None  # lazy

    def _init(self):
        segs = self.segs
        G = segs[0][1]
        for d in range(1, len(segs)):
            G = np.expand_dims(G, -1) + segs[d][1]
        self.shape = G.shape
        flat = G.ravel()
        self.order = np.argsort(-flat, kind="stable")
        self.flat_scores = flat
        self.pos = 0

    def __iter__(self):
        return self

    def __next__(self):
        if self.pos is None:
            self._init()
        if self.pos >= len(self.order):
            raise StopIteration
        o = self.order[self.pos]
        self.pos += 1
        idx = np.unravel_index(o, self.shape)
        cand = b"".join(self.segs[k][0][idx[k]] for k in range(len(self.segs)))
        return (float(self.flat_scores[o]), cand)


def make_gen(segs):
    prod = 1
    for s in segs:
        prod *= len(s[0])
        if prod > GRID_MAX:
            return HeapGen(segs)
    return GridGen(segs)


out = sys.stdout.buffer
emitted = 0
seen = set() if DEDUP else None
BUF = []


def emit(cand):
    global emitted
    if DEDUP:
        if cand in seen:
            return 0
        seen.add(cand)
    BUF.append(cand)
    emitted += 1
    if len(BUF) >= 200_000:
        out.write(b"\n".join(BUF) + b"\n")
        BUF.clear()
    return 1


if MODE == "global":
    tpl_total = sum(c for c, _ in tpls)
    gens = []
    for c, segs in tpls:
        if c == 0 or tpl_total == 0:
            continue  # zero prior: never wins the merge (and log(0) is undefined)
        off = math.log(c / tpl_total)  # template prior: joint P = P(tpl) * P(fill|tpl)
        g = make_gen(segs)
        gens.append(((s + off, cand) for s, cand in g))
    for score, cand in heapq.merge(*gens, key=lambda t: t[0], reverse=True):
        if emitted >= LIMIT:
            break
        emit(cand)
else:  # budget (legacy): per-template quota proportional to train frequency;
    # only really emitted candidates burn quota (dups skipped), quotas round
    # up, and unused quota of exhausted templates is redistributed each round
    gens = [make_gen(segs) for _, segs in tpls]
    # zero-count templates get no quota share; excluding them keeps total > 0
    live = [t for t in range(len(tpls)) if tpls[t][0] > 0]
    while live and emitted < LIMIT:
        total = sum(tpls[t][0] for t in live)
        for t in list(live):
            if emitted >= LIMIT:
                break
            budget = int(math.ceil(LIMIT * (tpls[t][0] / total)))
            got = 0
            for score, cand in gens[t]:
                if got >= budget or emitted >= LIMIT:
                    break
                got += emit(cand)
            if got < budget and emitted < LIMIT:  # generator ran dry
                live.remove(t)

if BUF:
    out.write(b"\n".join(BUF) + b"\n")
    BUF.clear()
out.flush()
print("emitted: %d" % emitted, file=sys.stderr)
