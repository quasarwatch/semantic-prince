#!/usr/bin/env python3
"""Dump post-injection counts/logu/u for targeted parity runs.

Replicates the setup phase of src/targeted.py (service injection with boost,
personal file reading with errors="ignore", strip, order-preserving dedup,
de-leet expansion, injection with TOPCNT) and writes a binary sidecar:

  [u64 K_final][i64 counts[K]][f64 logu[K]][f64 u[K]]

logu = np.log(counts/f64 sum) and u = np.exp(logu) computed by numpy, so the
C port can load bit-identical values (numpy SIMD exp/log may differ from
scalar libm by 1 ulp; see plan 5.3 / N7). The C side validates K and counts
against its own injection result before using the dumped floats.

Usage: dump_targeted_f64.py UNI BI K OUT [--service=NAME] [--personal=FILE]
       [--deleet]
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "src"))

import numpy as np  # noqa: E402
from spmodel import load_vocab_model  # noqa: E402

# data/ is read-only for the port effort: never write .cache_vocab_*.npz
np.savez = lambda *a, **k: None
np.savez_compressed = lambda *a, **k: None

pos = [a for a in sys.argv[1:] if not a.startswith("--")]
flags = [a for a in sys.argv[1:] if a.startswith("--")]

uni_path, bi_path, K, out_path = pos[0], pos[1], int(pos[2]), pos[3]


def flagval(name):
    for a in flags:
        if a.startswith(name + "="):
            return a.split("=", 1)[1]
    return None


SERVICE = flagval("--service")
PERSONAL_FILE = flagval("--personal")
DELEET = any(a == "--deleet" for a in flags)

SERVICE_VARIANTS = {
    "linkedin": ["linkedin", "linked", "lnkd", "mylinkedin"],
}


def service_variants(name):
    name = name.strip().lower()
    vs = list(dict.fromkeys(SERVICE_VARIANTS.get(name, [name, "my" + name])))
    vs.sort(key=len, reverse=True)
    return vs


LEET_TR = str.maketrans({"@": "a", "4": "a", "0": "o", "3": "e",
                         "1": "i", "!": "i", "$": "s", "5": "s",
                         "7": "t", "8": "b"})


def deleet_expand(ws):
    out, seen = [], set()
    for w in ws:
        for v in (w, w.translate(LEET_TR)):
            if v and v not in seen:
                seen.add(v)
                out.append(v)
    return out


m = load_vocab_model(uni_path, bi_path, K)
words = list(m.words)
widx = {w: i for i, w in enumerate(words)}
counts = list(int(c) for c in m.counts)
TOPCNT = counts[0]


def inject(ws, boost=False):
    for w in ws:
        i = widx.get(w)
        if i is None:
            widx[w] = len(words)
            words.append(w)
            counts.append(TOPCNT)
        elif boost:
            counts[i] = TOPCNT


if SERVICE:
    inject(service_variants(SERVICE), boost=True)

if PERSONAL_FILE:
    with open(PERSONAL_FILE, errors="ignore") as f:
        personal_words = [l.strip() for l in f if l.strip()]
    personal_words = list(dict.fromkeys(personal_words))
    if DELEET:
        personal_words = deleet_expand(personal_words)
    inject(personal_words)

counts = np.array(counts, dtype=np.int64)
logu = np.log(counts.astype(np.float64) / counts.sum())
u = np.exp(logu)

with open(out_path, "wb") as f:
    np.array([len(words)], dtype=np.uint64).tofile(f)
    counts.tofile(f)
    logu.tofile(f)
    u.tofile(f)

print("dumped K=%d -> %s" % (len(words), out_path), file=sys.stderr)
