#!/usr/bin/env python3
# Deterministic fixtures for C-vs-python parity testing of spg evaluate.
import os

D = os.path.dirname(os.path.abspath(__file__))

test_a = (
    b"alpha\r\n"
    b"beta\n"
    b"alpha\n"
    b"gamma delta\r\n"
    b"\n"
    b"   \r\n"
    b"\t\n"
    b"\x0b\x0c\r\n"
    b"  padded  \r\n"
    b"tab\ttab\n"
    b"crlf\n"
    b"crlf\r\n"
    b"zeta"
)
cand_a = (
    b"alpha\n"
    b"alpha\r\n"
    b"nope1\n"
    b"\n"
    b"beta\n"
    b"beta\n"
    b"gamma delta\r\n"
    b"  padded  \n"
    b"  padded\n"
    b"tab\ttab\r\n"
    b"zeta\r\n"
    b"nope2\n"
    b"nope3"
)

with open(os.path.join(D, "test_small.txt"), "wb") as f:
    f.write(test_a)
with open(os.path.join(D, "cand_small.txt"), "wb") as f:
    f.write(cand_a)

with open(os.path.join(D, "test_big.txt"), "wb") as f:
    for i in range(1500):
        end = b"\r\n" if i % 3 == 0 else b"\n"
        f.write(b"pw%06d" % i + end)
    f.write(b"pw000000\r\n")
    f.write(b"\n")

with open(os.path.join(D, "cand_big.txt"), "wb") as f:
    for i in range(1_100_000):
        if i % 977 == 0:
            hit = b"pw%06d" % ((i // 977) % 1500)
            f.write(hit + (b"\r\n" if i % 2 else b"\n"))
        elif i % 5 == 0:
            f.write(b"\r\n")
        else:
            f.write(b"miss%08d\n" % i)

print("fixtures written")
