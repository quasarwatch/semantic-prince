#!/usr/bin/env python3
"""Run src/targeted.py with np.savez stubbed out, so the reference never
writes .cache_vocab_*.npz into data/ (data/ stays read-only for the port).

Usage: run_targeted_ref.py UNI BI TEST [NP] [K] [flags...]
All arguments are forwarded verbatim.
"""
import os
import runpy
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src")

sys.path.insert(0, SRC)

import numpy as np  # noqa: E402

np.savez = lambda *a, **k: None
np.savez_compressed = lambda *a, **k: None

TARGET = os.path.join(SRC, "targeted.py")
sys.argv = [TARGET] + sys.argv[1:]

try:
    runpy.run_path(TARGET, run_name="__main__")
except SystemExit as e:
    sys.exit(0 if e.code is None else e.code)
