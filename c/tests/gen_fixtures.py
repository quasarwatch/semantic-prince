#!/usr/bin/env python3
# gen_fixtures.py -- regenerates all mini-fixtures for the C/Python parity
# harness, byte-exact, without depending on /tmp/rt* (which is volatile).
#
# Usage:
#   gen_fixtures.py            # write fixtures into tests/fixtures/
#   gen_fixtures.py --check    # verify on-disk fixtures match the generator
#
# Fixture provenance:
#   rt/   -- redteam v1 fixtures (bugfixes_v1.md, bugs B1..B7)
#   rt2/  -- redteam v2 fixtures (bugfixes_v2.md, bugs H1,H3,H4,H7,H9)
#   mini/ -- synthetic small dictionary (30 words) for L1 smoke parity
#
# Golden outputs (*.txt under rt/ and rt2/ that are NOT generated here) were
# copied from /tmp/rt and /tmp/rt2 on 2026-11-14 and are inputs to
# parity.sh golden checks; they are not regenerated (they are outputs of
# the fixed Python code, re-verified live by the harness).

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FX = os.path.join(HERE, "fixtures")

W = {}  # relpath -> bytes


def put(rel, text):
    W[rel] = text.encode("utf-8")


def put_bytes(rel, data):
    W[rel] = data


def tsv(rows):
    return "".join("\t".join(str(c) for c in r) + "\n" for r in rows)


# ---------------------------------------------------------------- rt/ (v1)
# 4-word dictionary + 2 bigrams: B1 budget fixture (16 unique candidates).
put("rt/uni4.tsv", tsv([("aa", 40), ("bb", 30), ("cc", 20), ("dd", 10)]))
put("rt/bi4.tsv", tsv([("aa bb", 5), ("bb aa", 4)]))
put("rt/tpl.tsv", tsv([("A4", 60), ("A2A2", 40)]))
put("rt/dig.tsv", "1\t7\t100\n2\t77\t50\n")
put("rt/sym.tsv", "1\t!\t100\n")

# 10-word dictionary: pairs/chains/targeted fixtures (B2,B3,B4,B5,H4).
put("rt/uni.tsv", tsv([
    ("ab", 100), ("abc", 100), ("aba", 90), ("bc", 90), ("xx", 100),
    ("cxx", 1), ("aa", 80), ("bb", 70), ("cc", 60), ("dd", 50),
]))
put("rt/bi.tsv", tsv([
    ("abc xx", 50), ("ab abc", 40), ("aba bc", 30), ("aa bb", 20),
]))
put("rt/test_b4.txt", "abcxx\nabcaba\n")
put("rt/personal.txt", "ab\nabc\n")
put("rt/leet.txt", "1v@n\np@ssw0rd\n")
put("rt/test1.txt", "abcxx\n")

# ---------------------------------------------------------------- rt2/ (v2)
# Zero-count templates: H1 (ZeroDivisionError / log(0)).
put("rt2/tplZ.tsv", tsv([("A2", 0), ("A4", 0)]))
put("rt2/tplZ2.tsv", tsv([("A2", 0), ("A4", 10)]))
put("rt2/tplR.tsv", tsv([("A2", 1000), ("A4", 1)]))
put("rt2/dig.tsv", "1\t7\t100\n2\t77\t50\n")
put("rt2/sym.tsv", "1\t!\t100\n")

# Second 10-word dictionary for H4 chains regression.
put("rt2/uni10.tsv", tsv([
    ("a", 40), ("b", 30), ("c", 20), ("ab", 15), ("aba", 12),
    ("bc", 10), ("abc", 9), ("x", 8), ("xx", 7), ("xa", 6),
]))
put("rt2/bi10.tsv", tsv([("a b", 5), ("ab c", 4)]))

# Tiny 3-word dictionary (tie-heavy).
put("rt2/uni3.tsv", tsv([("aa", 10), ("aaa", 10), ("bb", 10)]))
put("rt2/bi3.tsv", tsv([("a b", 1)]))

# H7: CRLF test set vs LF candidate stream (and reverse direction).
put_bytes("rt2/crlf_test.txt", b"abc\r\ndef\r\n")
put("rt2/stream_abcd.txt", "abc\ndef\n")        # LF stream vs CRLF set
put_bytes("rt2/stream_crlf.txt", b"abcxx\r\n")  # CRLF stream vs LF set

# H9: duplicate personal words.
put("rt2/pers_dup.txt", "ab\nab\nabc\n")
put("rt2/test_dup.txt", "abcxx\nabcxx\nabcaba\n")

# ---------------------------------------------------------------- mini/ (L1)
# Synthetic 30-word dictionary, counts strictly descending, lengths 2..6.
MINI_WORDS = [
    ("love", 1000), ("pass", 950), ("word", 900), ("star", 850),
    ("moon", 800), ("sun", 750), ("cat", 700), ("dog", 650),
    ("fish", 600), ("bird", 550), ("red", 500), ("blue", 450),
    ("green", 400), ("black", 350), ("white", 300), ("king", 290),
    ("queen", 280), ("jack", 270), ("ace", 260), ("fire", 250),
    ("ice", 240), ("rain", 230), ("snow", 220), ("wind", 210),
    ("rock", 200), ("tree", 190), ("leaf", 180), ("gold", 170),
    ("silver", 160), ("iron", 150),
]
put("mini/pw_uni.tsv", tsv(MINI_WORDS))

MINI_PAIRS = [
    ("love", "cat", 90), ("cat", "dog", 85), ("pass", "word", 80),
    ("sun", "moon", 75), ("star", "love", 70), ("red", "sun", 65),
    ("blue", "moon", 60), ("king", "queen", 55), ("queen", "jack", 50),
    ("fire", "ice", 48), ("rain", "snow", 46), ("wind", "rain", 44),
    ("rock", "gold", 42), ("gold", "fish", 40), ("fish", "bird", 38),
    ("black", "cat", 36), ("white", "bird", 34), ("iron", "man", 0),
]
# drop pairs with words missing from the dictionary ("man" is absent)
MINI_PAIRS = [(a, b, c) for (a, b, c) in MINI_PAIRS if c > 0]
put("mini/pw_bi.tsv", tsv(MINI_PAIRS))

# Small template/digit/symbol tables reused from rt semantics.
put("mini/tpl.tsv", tsv([("A4", 500), ("A2A2", 300), ("A3D2", 120),
                         ("A4D1", 100), ("D2A3", 80), ("A2S1A2", 40)]))
put("mini/dig.tsv", tsv([(1, "1", 1000), (1, "2", 800), (1, "7", 600),
                         (2, "12", 500), (2, "77", 400), (2, "99", 300)]))
put("mini/sym.tsv", tsv([(1, "!", 900), (1, "@", 700), (1, "*", 300)]))

put("mini/test.txt", "love1\ncatdog\npass12\nsunmoon99\nred777\n"
                     "blue!ice\nking77\nzzz\n")
put("mini/personal.txt", "love\nsun\n")
put("mini/stream.txt", "love1\nabc\ncatdog\n")  # evaluate stdin fixture


def main():
    check = "--check" in sys.argv
    n_wr = n_ok = n_bad = 0
    for rel in sorted(W):
        path = os.path.join(FX, rel)
        data = W[rel]
        if check:
            try:
                with open(path, "rb") as f:
                    cur = f.read()
            except OSError:
                cur = None
            if cur == data:
                n_ok += 1
            else:
                n_bad += 1
                print("MISMATCH: %s" % path)
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as f:
                f.write(data)
            n_wr += 1
    if check:
        print("check: %d ok, %d mismatched" % (n_ok, n_bad))
        return 1 if n_bad else 0
    print("wrote %d fixture files into %s" % (n_wr, FX))
    return 0


if __name__ == "__main__":
    sys.exit(main())
