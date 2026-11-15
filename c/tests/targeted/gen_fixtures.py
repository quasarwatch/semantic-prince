#!/usr/bin/env python3
"""Generate targeted-mode fixtures into c/tests/targeted/fixtures/.

Two models:
  small: 40 words (incl. one multi-byte word "čćx"), hand-picked bigrams,
         a test set exercising unicode isalpha, errors="ignore", CRLF/lone-CR
         line endings, duplicates (latent dedup), all-split hits (B4);
  med:   1500 words (the 40 small ones + w0000..w1459), deterministic
         bigrams, 50-word personal list; with K=1200 the candidate space
         is 120K+ so the 100K checkpoint fires.
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
FX = os.path.join(HERE, "fixtures")

SMALL_WORDS = [
    ("the", 1000), ("and", 900), ("love", 850), ("abc", 800), ("ab", 700),
    ("xyz", 600), ("qwerty", 500), ("pass", 450), ("word", 400), ("aba", 350),
    ("xx", 300), ("caba", 250), ("cxx", 240), ("lnkd", 230), ("linked", 220),
    ("zorn", 210), ("password", 200), ("sara", 190), ("quenat", 180),
    ("harn", 170), ("veld", 160), ("ossa", 150), ("elena", 140), ("dave", 130),
    ("katie", 120), ("laura", 110), ("pylm", 105), ("home", 100), ("work", 95),
    ("sun", 90), ("moon", 85), ("star", 80), ("sky", 75), ("cat", 70),
    ("dog", 65), ("fox", 60), ("owl", 55), ("čćx", 50), ("ivanov", 45),
    ("abcxx", 44),
]

SMALL_BI = [
    ("ab", "xx", 30), ("ab", "abc", 25), ("abc", "xx", 20),
    ("zorn", "pass", 15), ("pass", "word", 12), ("sara", "love", 10),
    ("xx", "ab", 8), ("abc", "aba", 7), ("lnkd", "ab", 5), ("ab", "lnkd", 4),
    ("čćx", "ab", 3), ("ab", "čćx", 2), ("harn", "veld", 6),
    ("quenat", "sara", 9), ("password", "zorn", 11), ("zorn", "password", 13),
    ("owl", "fox", 1), ("the", "and", 40), ("and", "the", 38),
    ("love", "abc", 22),
]

# last line deliberately has NO trailing newline; contains: CRLF line,
# lone-CR split, invalid UTF-8 bytes, Cyrillic, mixed-script alpha lines
SMALL_TEST = (
    b"abcxx\n"
    b"abcaba\n"
    b"ab\xc4\x8d\xc4\x87x\n"          # ab + čćx  (hit dir0)
    b"ab\xc4\x8dx\n"                  # abčx: rem 2 codepoints -> NOT qualified
    b"\xc4\x8d\xc4\x87xab\n"          # čćxab (hit dir1)
    b"xxab\n"                         # rem 2 -> no
    b"abcdef\n"                       # qualified, unreachable
    b"xyz\n"
    b"ab\n"
    b"\n"
    b"   \n"
    b"ABABC\n"
    b"abcxx\n"                        # duplicate (latent dedup)
    b"abcaba\r\n"                     # CRLF duplicate
    b"ab\xff\xfecxx\n"                # errors=ignore -> "abcxx" again
    b"xyz\rabcxx\n"                   # lone CR splits into xyz / abcxx
    b"\xd0\xb8\xd0\xb2\xd0\xb0\xd0\xbd\xd0\xbe\xd0\xb2\n"  # иванов: alpha, no match
    b"abc\xd0\xb0\xd0\xb1\xd0\xb2\n"  # abc + абв: qualified, unreachable
    b"\xd0\xb0\xd0\xb1\xd0\xb2abc\n"  # абвabc: qualified, unreachable
    b"passwordabc\n"
    b"saraaba\n"
    b"ivanaba\n"
    b"zorn\n"
    b"ivanab\n"
    b"qqabc\n"
    b"ab\xc3\xa9xx\n"                 # abéxx: alpha, qualified, unreachable
    b"lnkdabc"                        # no trailing newline
)

SMALL_PERSONAL = (
    b"ab\nabc\nivan\nab\n\n   \n  sara  \nxyz\nunknownword\n"
)

SMALL_LEET = (
    b"1v@n\np@ssw0rd\n$ara7\nabc\n1v@n\n"
)


def write(name, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(os.path.join(FX, name), mode) as f:
        f.write(data)


def main():
    os.makedirs(FX, exist_ok=True)

    # ---- small model ----
    write("uni.tsv", "".join("%s\t%d\n" % w for w in SMALL_WORDS))
    write("bi.tsv", "".join("%s %s\t%d\n" % t for t in SMALL_BI))
    write("test.txt", SMALL_TEST)
    write("personal.txt", SMALL_PERSONAL)
    write("personal_leet.txt", SMALL_LEET)

    # ---- medium model: 40 small words + w0000..w1459 (1500 total) ----
    med_words = [(w, c + 100000) for w, c in SMALL_WORDS]
    for i in range(1460):
        med_words.append(("w%04d" % i, 60000 - i * 37))
    write("uni_med.tsv", "".join("%s\t%d\n" % w for w in med_words))

    small_names = [w for w, _ in SMALL_WORDS]
    med_names = ["w%04d" % i for i in range(1460)]
    pairs = set()
    lines = []
    for i in range(4000):
        a = small_names[i % len(small_names)]
        b = med_names[(i * 37) % 1100]
        if (a, b) in pairs:
            continue
        pairs.add((a, b))
        lines.append("%s %s\t%d\n" % (a, b, (i * 7) % 500 + 1))
    for i in range(1500):
        a = med_names[(i * 53) % 1100]
        b = small_names[(i * 11) % len(small_names)]
        if (a, b) in pairs:
            continue
        pairs.add((a, b))
        lines.append("%s %s\t%d\n" % (a, b, (i * 13) % 700 + 1))
    write("bi_med.tsv", "".join(lines))

    # ---- 50 personal words: 10 small + 36 medium + 4 injected ----
    p50 = ["ab", "abc", "zorn", "sara", "password", "quenat", "harn", "veld",
           "ossa", "elena"]
    p50 += ["w%04d" % i for i in range(36)]
    p50 += ["notinvocab1", "notinvocab2", "notinvocab3", "notinvocab4"]
    p50[7] = "ab"      # duplicate -> dedup must drop the second "ab"
    p50.append("veld")  # trailing duplicate
    assert len(p50) == 51
    write("personal50.txt", "".join(w + "\n" for w in p50))

    # ---- medium test set ----
    all_names = small_names + med_names
    tlines = []
    for i in range(3000):
        w = p50[i % len(p50)]
        x = all_names[(i * 17) % 1190]
        tlines.append(w + x if i % 2 == 0 else x + w)
    tlines += ["12345", "abc123", "", "   ", "иванов", "ab" + "q" * 12]
    tlines += tlines[:100]          # duplicates (latent dedup accounting)
    write("test_med.txt", ("\n".join(tlines) + "\n").encode())

    print("fixtures written to %s" % FX)


if __name__ == "__main__":
    main()
