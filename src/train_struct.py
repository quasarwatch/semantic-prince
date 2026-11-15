#!/usr/bin/env python3
# Train structure templates: skeleton of each password as class+length segments.
#   ivan1992pet! -> A4D4A3S1
# Counts: template frequencies, digit-value frequencies per run length,
# symbol-string frequencies per run length.
# Usage: train_struct.py train.txt out_prefix

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
import re
from collections import Counter

if "--help" in sys.argv or len([a for a in sys.argv[1:] if not a.startswith("--")]) < 2:
    sys.stderr.write("usage: train_struct.py TRAIN.txt OUT_PREFIX\n");
    sys.exit(1)
train_path, out_prefix = sys.argv[1], sys.argv[2]

seg_re = re.compile(r"[a-z]+|[0-9]+|[^a-z0-9]+")

templates = Counter()
digits = {}   # length -> Counter of values
symbols = {}  # length -> Counter of strings

with open(train_path, errors="ignore") as f:
    for line in f:
        pw = line.rstrip("\n").lower()
        if not pw or len(pw) > 40:
            continue
        segs = seg_re.findall(pw)
        if not segs:
            continue
        tpl = []
        ok = True
        for s in segs:
            if s[0].isalpha():
                tpl.append("A%d" % min(len(s), 16))
            elif s[0].isdigit():
                l = len(s)
                if l > 8:
                    ok = False
                    break
                tpl.append("D%d" % l)
                digits.setdefault(l, Counter())[s] += 1
            else:
                l = len(s)
                if l > 3:
                    ok = False
                    break
                tpl.append("S%d" % l)
                symbols.setdefault(l, Counter())[s] += 1
        if ok and len(tpl) <= 5:
            templates["".join(tpl)] += 1

with open(out_prefix + "_templates.tsv", "w") as f:
    for t, c in templates.most_common():
        f.write("%s\t%d\n" % (t, c))

with open(out_prefix + "_digits.tsv", "w") as f:
    for l in sorted(digits):
        for v, c in digits[l].most_common(2000):
            f.write("%d\t%s\t%d\n" % (l, v, c))

with open(out_prefix + "_syms.tsv", "w") as f:
    for l in sorted(symbols):
        for v, c in symbols[l].most_common(100):
            f.write("%d\t%s\t%d\n" % (l, v, c))

print("templates: %d, digit lens: %s, sym lens: %s" % (
    len(templates), sorted(digits), sorted(symbols)), file=sys.stderr)
