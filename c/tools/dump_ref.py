#!/usr/bin/env python3
# Reference dump for the C parity gate: prints "name sha256" per model array,
# exactly the format of "spg dump". Read-only on data/: np.savez is patched
# out so a cache miss never writes a new npz.
import sys, os, json, hashlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "src"))

import numpy as np
import spmodel

np.savez = lambda *a, **k: None  # keep data/ read-only

uni, bi, K = sys.argv[1], sys.argv[2], int(sys.argv[3])

m = spmodel.load_vocab_model(uni, bi, K)

sig = json.dumps(spmodel._sig([uni, bi]))

def h(b):
    return hashlib.sha256(b).hexdigest()

print("sig", h(sig.encode("utf-8")))
print("words_blob", h(b"\n".join(m.wb)))
print("counts", h(m.counts.astype("<i8").tobytes()))
print("bi_i", h(m.bi_i.astype("<i4").tobytes()))
print("bi_j", h(m.bi_j.astype("<i4").tobytes()))
print("bi_cnt", h(m.bi_cnt.astype("<i8").tobytes()))
print("logu", h(m.logu.astype("<f8").tobytes()))
