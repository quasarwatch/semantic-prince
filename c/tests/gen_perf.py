#!/usr/bin/env python3
# 33M-line candidate stream: ~10% hits (cycling the real test set, so dup
# hits occur), rest misses. Writes perf_stream.txt next to itself.
import os

D = os.path.dirname(os.path.abspath(__file__))

with open("$(pwd)/../data/test.txt", "rb") as f:
    testset = [l.rstrip(b"\r\n") for l in f if l.strip()]

N = 33_000_000
out = open(os.path.join(D, "perf_stream.txt"), "wb")
buf = bytearray()
for i in range(N):
    if i % 10 == 0:
        buf += testset[(i // 10) % len(testset)] + b"\n"
    else:
        buf += b"zz" + str(i).encode() + b"q\n"
    if len(buf) > 8_000_000:
        out.write(buf)
        buf.clear()
out.write(buf)
out.close()
print("done", N)
