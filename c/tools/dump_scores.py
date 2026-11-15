#!/usr/bin/env python3
# Dump logu[K] and logp[K*K] (pwbi, Jelinek-Mercer) as raw little-endian
# float64 for the C port's parity fallback (c_port_plan.md 5.3):
# numpy SIMD log and glibc libm log differ by <=1 ulp on ~4e-6 of the
# inputs, so exact bitwise parity runs feed these dumps to
# "spg pairs" via SP_SCORES_BIN=<file>.
# Layout: logu (K f64) then logp (K*K f64), C-order (i*K+j).
# Usage: dump_scores.py pw_uni.tsv pw_bi.tsv K alpha out.bin
import sys
import numpy as np
from spmodel import load_vocab_model

uni_path, bi_path, K, alpha, out_path = (
    sys.argv[1], sys.argv[2], int(sys.argv[3]), float(sys.argv[4]), sys.argv[5])

m = load_vocab_model(uni_path, bi_path, K)
K = len(m.words)

with open(out_path, "wb") as f:
    m.logu.tofile(f)

    M = np.zeros((K, K))
    M[m.bi_i, m.bi_j] = m.bi_cnt
    cf = m.counts.astype(np.float64)
    pcond = M / cf[:, None]
    del M
    pjm = alpha * pcond + (1 - alpha) * (cf / cf.sum())[None, :]
    del pcond
    logp = np.log(np.maximum(pjm, 1e-15))
    del pjm
    logp.tofile(f)

print("dumped logu[%d] + logp[%d] -> %s" % (K, K * K, out_path), file=sys.stderr)
