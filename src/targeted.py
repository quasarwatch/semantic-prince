#!/usr/bin/env python3
# Targeted-attack demo v2: attacker knows ONE word from victim's password (OSINT).
# Personal words = top-N tokens. Test slice = test passwords starting/ending with
# a personal word, alpha remainder len>=3. Two orderings of the SAME pair space:
#   freq: order x by unigram freq
#   lm:   order x by P_jm(x|w) bigram (alpha=0.7)
# Fixes vs v1: bigram mass aggregates computed once (was O(NP*K*|bi|), ~81h),
# reverse direction normalized by out-mass of x (was in-mass of w).
# Metric: cumulative unique hits on the slice at checkpoints.
# Usage: targeted.py pw_uni.tsv pw_bi.tsv test.txt [N_personal] [K_vocab] [flags]
# Flags:
#   --service=NAME    OSINT-режим: имя сервиса + варианты (linkedin ->
#                     linkedin/linked/lnkd/mylinkedin) встраиваются в словарь
#                     с высоким приоритетом и в начало personal-списка
#   --personal=FILE   личные слова из файла (1/строка) вместо top-NP
#   --deleet          при загрузке личных слов распознавать leet-формы:
#                     1v@n -> добавить и "ivan", и оригинал
#                     (@/4→a, 0→o, 3→e, 1/!→i, $/5→s, 7→t, 8→b)
#   --emit=w1,w2,...  stream w+x / x+w candidates for known victim words
import sys, math
import numpy as np
from spmodel import load_vocab_model

pos = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]
uni_path, bi_path, test_path = pos[0], pos[1], pos[2]
NP = int(pos[3]) if len(pos) > 3 else 200
K = int(pos[4]) if len(pos) > 4 else 10000
ALPHA = 0.7


def flagval(name):
    for a in flags:
        if a.startswith(name + "="):
            return a.split("=", 1)[1]
    return None


SERVICE = flagval("--service")          # --service=linkedin
PERSONAL_FILE = flagval("--personal")   # --personal=words.txt
DELEET = any(a == "--deleet" for a in flags)

# варианты имён известных сервисов; неизвестное имя -> [name, my+name]
SERVICE_VARIANTS = {
    "linkedin": ["linkedin", "linked", "lnkd", "mylinkedin"],
}


def service_variants(name):
    name = name.strip().lower()
    vs = list(dict.fromkeys(SERVICE_VARIANTS.get(name, [name, "my" + name])))
    vs.sort(key=len, reverse=True)  # длинные первыми: корректный разбор префиксов
    return vs


# de-leet входа: @/4→a, 0→o, 3→e, 1/!→i, $/5→s, 7→t, 8→b
LEET_TR = str.maketrans({"@": "a", "4": "a", "0": "o", "3": "e",
                         "1": "i", "!": "i", "$": "s", "5": "s",
                         "7": "t", "8": "b"})


def deleet_expand(ws):
    """Каждое слово + его de-leet форма (если отличается), без дублей."""
    out, seen = [], set()
    for w in ws:
        for v in (w, w.translate(LEET_TR)):
            if v and v not in seen:
                seen.add(v)
                out.append(v)
    return out


m = load_vocab_model(uni_path, bi_path, K)
words, wb, counts, widx = m.words, m.wb, m.counts, m.widx
TOPCNT = int(counts[0])  # счётчик топ-слова = «высокий приоритет» инъекций


def inject(ws, boost=False):
    """Гарантирует присутствие слов в словаре, возвращает их индексы.
    Отсутствующие слова добавляются в конец со счётчиком топ-слова
    (высокий приоритет); boost=True поднимает счётчик и у уже
    присутствующих слов."""
    global counts
    idxs, add = [], []
    for w in ws:
        i = widx.get(w)
        if i is None:
            i = len(words)
            widx[w] = i
            words.append(w)
            wb.append(w.encode())
            add.append(TOPCNT)
        elif boost:
            counts[i] = TOPCNT
        idxs.append(i)
    if add:
        counts = np.concatenate([counts, np.array(add, dtype=np.int64)])
    return idxs


service_idxs = []
if SERVICE:
    service_idxs = inject(service_variants(SERVICE), boost=True)
    print("service %r -> %s" % (SERVICE, [words[i] for i in service_idxs]),
          file=sys.stderr)

personal_idxs = None
if PERSONAL_FILE:
    with open(PERSONAL_FILE, errors="ignore") as f:
        personal_words = [l.strip() for l in f if l.strip()]
    n_raw = len(personal_words)
    personal_words = list(dict.fromkeys(personal_words))  # dedup, keep order
    if DELEET:
        personal_words = deleet_expand(personal_words)
    print("personal file %s: %d words%s -> %d"
          % (PERSONAL_FILE, n_raw, " +deleet" if DELEET else "",
             len(personal_words)), file=sys.stderr)
    personal_idxs = inject(personal_words)

K = len(words)
logu = np.log(counts.astype(np.float64) / counts.sum())
u = np.exp(logu)  # unigram probs
seen_svc = set(service_idxs)
if personal_idxs is not None:
    personal = service_idxs + [i for i in personal_idxs if i not in seen_svc]
else:
    personal = service_idxs + [i for i in range(min(NP, K))
                               if i not in seen_svc]
pwb = [words[i] for i in personal]
pt = tuple(pwb)

# bigram aggregates, computed once
out_mass = np.zeros(K)
np.add.at(out_mass, m.bi_i, m.bi_cnt)

# --emit=w1,w2,...: stream w+x / x+w candidates for known victim words, lm order
emit_spec = [a for a in flags if a == "--emit" or a.startswith("--emit=")]
if emit_spec:
    spec = emit_spec[0]
    ew = spec.split("=", 1)[1].split(",") if "=" in spec else [words[i] for i in personal]
    if DELEET:
        ew = deleet_expand(ew)
    cands = []
    for w in ew:
        if w not in widx:
            print("not in vocab: %s" % w, file=sys.stderr)
            continue
        wi = widx[w]
        cond_f = np.zeros(K)
        rowmask = m.bi_i == wi
        if rowmask.any():
            np.add.at(cond_f, m.bi_j[rowmask],
                      m.bi_cnt[rowmask] / max(out_mass[wi], 1e-15))
        s_f = logu[wi] + np.log(np.maximum(ALPHA * cond_f + (1 - ALPHA) * u, 1e-15))
        cond_r = np.zeros(K)
        colmask = m.bi_j == wi
        if colmask.any():
            xs = m.bi_i[colmask]
            cond_r[xs] = m.bi_cnt[colmask] / np.maximum(out_mass[xs], 1e-15)
        s_r = logu + np.log(np.maximum(ALPHA * cond_r + (1 - ALPHA) * u[wi], 1e-15))
        for x in range(K):
            if x == wi:
                continue
            cands.append((s_f[x], wb[wi] + wb[x]))
            cands.append((s_r[x], wb[x] + wb[wi]))
    cands.sort(key=lambda t: -t[0])
    out = sys.stdout.buffer
    seen = set()
    buf = []
    for s, c in cands:
        if c not in seen:
            seen.add(c)
            buf.append(c)
        if len(buf) >= 100_000:
            out.write(b"\n".join(buf) + b"\n")
            buf.clear()
    if buf:
        out.write(b"\n".join(buf) + b"\n")
    out.flush()
    sys.exit(0)

# test slice: pw starts/ends with a personal word, remainder pure alpha len>=3
# hitkey = ((w_idx * K) + x_idx) * 2 + dir, dir 0 = w+x, 1 = x+w
hit_pw = {}   # hitkey -> password (unique-hit accounting by pw id)
              # NB: first line with a given split wins; a duplicate test line
              # counts into slice_size but can never become a hit (latent)
slice_size = 0
with open(test_path, errors="ignore") as f:
    for pwid, line in enumerate(f):
        pw = line.rstrip("\n")
        if not pw.isalpha():
            continue
        if not (pw.startswith(pt) or pw.endswith(pt)):
            continue
        qualified = False
        for wi, w in zip(personal, pwb):  # all splits: hit counts at its best rank
            if pw.startswith(w) and len(pw) - len(w) >= 3 and pw != w:
                qualified = True
                x = pw[len(w):]
                xi = widx.get(x)
                if xi is not None:
                    hit_pw.setdefault(((wi * K) + xi) * 2 + 0, pwid)
        for wi, w in zip(personal, pwb):
            if pw.endswith(w) and len(pw) - len(w) >= 3 and pw != w:
                qualified = True
                x = pw[:-len(w)]
                xi = widx.get(x)
                if xi is not None:
                    hit_pw.setdefault(((wi * K) + xi) * 2 + 1, pwid)
        if qualified:
            slice_size += 1
# reachable counts qualifying split registrations, not unique passwords:
# one password with two qualifying splits counts twice (may exceed slice)
print("personal words: %d, test slice: %d (reachable: %d)" %
      (len(personal), slice_size, len(hit_pw)), file=sys.stderr)

# candidate scores, vectorized per personal word
n_cand = len(personal) * K * 2
keys = np.fromiter((((wi * K) + xi) * 2 + d
                    for wi in personal for xi in range(K) for d in (0, 1)),
                   dtype=np.int64, count=n_cand)
scores = np.empty(n_cand, dtype=np.float64)

pos = 0
for wi in personal:
    # forward: w+x, score = logu[w] + log(ALPHA*C(w,x)/out_mass[w] + (1-ALPHA)*u(x))
    cond_f = np.zeros(K)
    rowmask = m.bi_i == wi
    if rowmask.any():
        np.add.at(cond_f, m.bi_j[rowmask],
                  m.bi_cnt[rowmask] / max(out_mass[wi], 1e-15))
    p_f = ALPHA * cond_f + (1 - ALPHA) * u
    s_f = logu[wi] + np.log(np.maximum(p_f, 1e-15))
    # reverse: x+w, score = logu[x] + log(ALPHA*C(x,w)/out_mass[x] + (1-ALPHA)*u(w))
    cond_r = np.zeros(K)
    colmask = m.bi_j == wi
    if colmask.any():
        xs = m.bi_i[colmask]
        cond_r[xs] = m.bi_cnt[colmask] / np.maximum(out_mass[xs], 1e-15)
    p_r = ALPHA * cond_r + (1 - ALPHA) * u[wi]
    s_r = logu + np.log(np.maximum(p_r, 1e-15))
    block = np.empty(K * 2)
    block[0::2] = s_f
    block[1::2] = s_r
    block[wi * 2:wi * 2 + 2] = -np.inf  # x == w excluded
    scores[pos:pos + K * 2] = block
    pos += K * 2

checkpoints = [100_000, 1_000_000, 10_000_000, 50_000_000]
for label, sc in (("lm", scores), ("freq", None)):
    if sc is None:
        # freq order: forward and reverse share logu[w]+logu[x]
        fs = np.empty(n_cand, dtype=np.float64)
        pos = 0
        for wi in personal:
            b = logu[wi] + logu
            blk = np.empty(K * 2)
            blk[0::2] = b
            blk[1::2] = b
            blk[wi * 2:wi * 2 + 2] = -np.inf
            fs[pos:pos + K * 2] = blk
            pos += K * 2
        sc = fs
    order = np.argsort(-sc, kind="stable")
    hits = set()
    ci = 0
    for n, o in enumerate(order, 1):
        pwid = hit_pw.get(int(keys[o]))
        if pwid is not None:
            hits.add(pwid)
        while ci < len(checkpoints) and n == checkpoints[ci]:
            print("%4s  %12d -> %7d cracked (%.2f%% of slice)" %
                  (label, n, len(hits), 100.0 * len(hits) / max(slice_size, 1)))
            ci += 1
    print("%4s  final %d -> %d (%.2f%% of slice)" %
          (label, n_cand, len(hits), 100.0 * len(hits) / max(slice_size, 1)))
