#!/usr/bin/env python3
# Streaming guess-curve evaluator: reads candidates from stdin, counts cumulative
# unique hits against a held-out test set, reports at checkpoints.
# Usage: generator | python3 -I evaluate.py test.txt [limit]
import sys

test_path = sys.argv[1]
limit = int(sys.argv[2]) if len(sys.argv) > 2 else 100_000_000
checkpoints = [100_000, 1_000_000, 10_000_000, 100_000_000]

with open(test_path, "rb") as f:
    testset = set(l.rstrip(b"\r\n") for l in f if l.strip())  # \r: CRLF files

print(f"test set: {len(testset)} unique passwords", file=sys.stderr)

seen_hits = set()
n = 0
ci = 0
buf = sys.stdin.buffer

for line in buf:
    cand = line.rstrip(b"\r\n")
    n += 1
    if cand in testset and cand not in seen_hits:
        seen_hits.add(cand)
    while ci < len(checkpoints) and n == checkpoints[ci]:
        print(f"{n:>12,} guesses -> {len(seen_hits):>8,} cracked ({100.0*len(seen_hits)/len(testset):.2f}%)")
        ci += 1
    if n >= limit:
        break

print(f"final: {n:,} guesses -> {len(seen_hits):,} cracked ({100.0*len(seen_hits)/len(testset):.2f}%)")
