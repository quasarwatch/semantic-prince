# Semantic PRINCE

A password candidate generator that orders guesses by how humans actually pick passwords.

In short: classic PRINCE (princeprocessor, `pp64`) glues dictionary words together in blind list order. Semantic PRINCE learns word habits from real disclosed passwords and emits candidates in probability order — the guesses a human would most likely pick come first. In clean cross-dump tests (the model never saw the target passwords) it cracks 1.6–3.2x more passwords than classic PRINCE for the same number of guesses, and outperforms PRINCE's full 100M-guess budget with 43M guesses.

## Where it fits

Semantic PRINCE is the layer *after* your dictionary. Real attacks always replay known plaintexts first — that is the strongest head there is. The generator earns its budget once the dictionary runs out. Measured on the raw LinkedIn dump (5.09M unique passwords, nothing removed):

| total guesses | rockyou + classic PRINCE | rockyou + Semantic PRINCE |
|---|---|---|
| 100K | 66,869 | 66,869 |
| 1M | 304,930 | 304,930 |
| 10M | 565,429 | 565,429 |
| ~54M | 705,256 | **803,674** (+14.0%) |

The rows are identical up to 10M because both stacks spend that budget on the same replay head (12.9M known passwords); the generators only differ afterwards. At an equal total budget the stack with Semantic PRINCE cracks 14% more; classic PRINCE needs 112M guesses to reach 706K.

Model-only (no dictionary head), the gap is wider: 41,006 vs 8,602 cracked at 100K guesses (x4.8) and 420,917 vs 202,971 at the end of a 43M vs 100M run (x2.07 with 43% of the budget).

## Overview

This project trains an n-gram model on a list of disclosed plaintext passwords and uses it to generate password guesses in probability order. Training is account-weighted: passwords are weighted by how many real accounts used them, so the head of the stream reflects real-world frequency, not just pattern variety. Three generator layers cover the shapes people actually use:

1. **struct** — word + tail patterns (`sunshine2024`, `dragon!`, `qwerty123`). The model knows which tails are common (year, `!`, `123`, `!23`) and which templates dominate, and splits the budget across templates by their real frequency instead of letting one hot template flood the queue.
2. **pairs** — word+word joins, ordered by a bigram model: what word tends to follow what in real passwords. Same candidate space as classic PRINCE, but in probability order. Given the dictionary `i, love, you, cat, car, soup`, classic PRINCE emits `ilove`, `iyou`, `icat`... in list order; the model emits `iloveyou` first and `soupcar` last, because nobody picks `soupcar`.
3. **chains** — three-word chains for long passphrases (beam search over bigram joins), reaching lengths that pairs cannot.

A **targeted mode** spins the generators around known words of a specific person or service: nicknames, pets, dates (`--personal words.txt`), service names with automatic variants (`--service linkedin` adds `linkedin`, `linked`, `lnkd`, `mylinkedin`), and leet-normalization of the input (`--deleet`: `p@ssw0rd` counts as both `p@ssw0rd` and `password`).

Like PCFG and classic PRINCE, this generator trades speed for accuracy: the pairs layer emits about 1.8M candidates per second per core (struct ~650K/s, chains ~40K/s — it runs a wide beam), so it is best used against slow hashing algorithms (scrypt, PBKDF2 with high iterations, wallet formats) or large salted lists, where each guess is expensive and accuracy pays for itself. For fast hashes at full GPU speed, classic mask/rule attacks still win on throughput.

The generator is not a replacement for hashcat's rule engine — it is the ordering brain in front of it. The recommended pipeline:

```
spg pairs ... | hashcat -r rules/best64.rule -m <mode> hashes.txt
```

case mutations, leet substitutions and symbol insertions are what rules do on the GPU for free; what to try first is what this tool answers.

## Requirements + Installation

**Python version (reference):**

```
pip install numpy
```

Python 3.10+. No GPU needed.

**C version (`spg`, recommended):**

```
cd c && make
```

gcc, libm, no other dependencies. The C port is bitwise-verified against the Python reference (41-case parity suite in `c/tests/`).

## Quick Start Guide

### With the prebuilt model

Download `sp-model-rockyou-v2.tar.gz` from Releases and unpack it next to the repo (it creates `data/`):

```
tar xzf sp-model-rockyou-v2.tar.gz
```

The default model was trained on the public RockYou dump, account-weighted by real usage counts (v2; v1 used an unweighted model).

A short attack (struct head + pairs):

```
c/spg struct data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 1000000 > short.txt
c/spg pairs  data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 5000000 >> short.txt
hashcat -m 0 -a 0 hashes.txt short.txt
```

The full campaign (long attack, 43M candidates):

```
c/spg struct  data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 14000000 >  campaign.txt
c/spg pairs   data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 25000000                                >> campaign.txt
c/spg chains  data/pw_uni.tsv data/pw_bi.tsv 10000  4000000 0.7 2000000 80                      >> campaign.txt
hashcat -m 0 -a 0 hashes.txt campaign.txt
```

Generation takes about two minutes with `spg`. All output is streamable — pipe straight into hashcat with `--stdin` if you do not want the file.

### Training on your own dump

Any password list works, one password per line. Two trainers build the model tables:

```
python3 src/train_password_ngrams.py mydump.txt data/count_1w.txt data/pw
python3 src/train_struct.py mydump.txt data/struct
```

The first writes `data/pw_uni.tsv` / `data/pw_bi.tsv` (word unigram/bigram counts; `count_1w.txt` is a reference web-ngram vocabulary used only to guide word segmentation, it ships in the data pack). The second writes the `data/struct_*.tsv` template tables. Both generators then pick up whatever is in `data/` — retraining is how you aim the tool at a specific language, community or leak. If your dump has usage counts, repeat each password accordingly (or concatenate several dumps) — the trainers count occurrences.

### Targeted mode

```
c/spg targeted data/pw_uni.tsv data/pw_bi.tsv test.txt 200 10000 --personal=words.txt --service=linkedin --deleet
```

`--personal` takes a file of the person's known words, one per line. `--service` injects the service name and its common variants at top priority. `--deleet` normalizes leet forms on input. For real-time candidates instead of a measured run, `--emit=w1,w2,...` streams candidates for specific pivot words.

### Scoring your own candidates

```
c/spg evaluate test.txt < candidates.txt
```

Counts unique hits at 100K / 1M / 10M / 100M guess checkpoints. Handles CRLF, duplicates and empty lines.

## Benchmarks

Model-only, measured against classic PRINCE (pp64 with the same top-5000 dictionary, `--pw-max=32`, fixed config) on clean cross-dump test sets. "Clean" means: unique passwords of the dump, minus everything present in the training set, minus case-fold leakage (a candidate whose lower() form is in the training set is removed from the test). The RockYou row is validation-only (same-family data); the headline number is the cross-dump row.

| test set | @100K | @1M | @10M | final |
|---|---|---|---|---|
| linkedin (clean, 4.36M) | 4,470 / 2,767 (**x1.62**) | 35,743 / 11,630 (**x3.07**) | 91,840 / 58,889 (**x1.56**) | **194,986 @ 43M** vs 104,797 @ 100M |
| rockyou (validation, 1.37M) | 4,718 / 2,101 (**x2.25**) | 29,149 / 9,010 (**x3.24**) | 66,960 / 41,596 (**x1.61**) | **143,950 @ 43M** vs 72,484 @ 100M |

(Each cell: Semantic PRINCE / pp64 cracked-count, ratio. Account-weighted — counting how many *accounts* the cracked passwords represent, not how many distinct strings — the same 43M campaign covers 11.5% of validation accounts at 100K guesses and 34.1% at the end: hits concentrate on high-multiplicity passwords.)

Full methodology, the exact pp64 command, the stack (with-dictionary) tables and the red-team audits: `docs/METHODOLOGY.md`.

Known caveats, documented honestly:

- leet-heavy passwords are out of scope by design (measured: a leet layer adds under 1K hits) — use hashcat rules for that
- non-ASCII passwords (CJK, emoji) are emitted only if present in the training data
- the model cannot invent words it never saw: out-of-vocabulary words need `--personal`

## Example output

Real output of each layer (RockYou v2 model, first lines):

```
$ c/spg pairs data/... 10000 pwbi 15     # first lines reflect RockYou-era email habits,
hotmailcom                               # so here are lines sampled from the first 2,000:
babyprincess
loveblue
sweetand
angelbaby
lovedaniel
lovegod
babyboy
thepassword
thereal
loveall
```

```
$ c/spg struct data/... 10000 12
monkey
nicole
rockyou
jessica
password
iloveyou
123456
butterfly
monkey12
angel
```

```
$ c/spg chains data/... 10000 8
qazwsxedc
ichliebedich
lovehotmailcom
girlyahoocom
loveyahoocom
girlhotmailcom
boyhotmailcom
zaqqazwsx
```

```
$ c/spg targeted data/... 200 10000 --personal=words.txt --emit
# words.txt: sunshine, qwerty
qwertyasdfgh
qwertyasd
qwertypop
passwordqwerty
qwertyqaz
qwertyabc
sunshinebaby
qwertykeyboard
qwertylol
qwertypoo
```

## FAQ

**Why not just rules + a big dictionary?** Rules mutate words; they do not order them. A 600M-word list with rules costs GPU years. Ordering tells you what to try first; rules tell you how to mutate it. They compose, they do not compete.

**Why not PCFG?** PCFG learns abstract skeletons (L4D4) and fills letter slots from a dictionary or a Markov chain. We measured against pcfg_cracker during development: its Markov fill covers wordless garbage passwords we skip, but on human-shaped passwords the word-anchored model wins on the budgets that matter. Different tools, different profiles.

**Why not just feed Cyclone/breach compilations?** Compilation wordlists win by memorizing the answers (they contain cracked passwords of the same breaches — circular). On targets the compilation never saw, a generative model wins. Both measurements are in `docs/METHODOLOGY.md`.

**Is this AI?** It is statistics: n-gram counts over real passwords, no neural network, no GPU, no magic. The whole model is a few TSV files you can inspect with a text editor.

## Repository layout

```
src/    Python reference implementation (trainer, generators, evaluator)
c/      the C port (spg binary, parity test harness in c/tests/)
docs/   benchmarks and methodology
tools/  helper scripts
```

## References

- PRINCE: PRINCEprocessor by Jens Steube, https://github.com/hashcat/princeprocessor
- PCFG: "PCFG Password Guess Generator" by Matt Weir et al., and lakiw/pcfg_cracker
- RockYou dump (2009): the standard public research wordlist this model trains on

## License

MIT
