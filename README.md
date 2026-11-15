# Semantic PRINCE

A password candidate generator that orders guesses by how humans actually pick passwords.

In short: classic PRINCE (princeprocessor, `pp64`) glues dictionary words together in blind list order. Semantic PRINCE learns word habits from real disclosed passwords and emits candidates in probability order — the guesses a human would most likely pick come first. In clean cross-dump tests (the model never saw the target passwords) it cracks about 2x more passwords than classic PRINCE for the same number of guesses, and outperforms PRINCE's full 100M-guess budget with 43M guesses.

## Overview

This project trains an n-gram model on a list of disclosed plaintext passwords and uses it to generate password guesses in probability order. Three generator layers cover the shapes people actually use:

1. **struct** — word + tail patterns (`ivan2010`, `maria!`, `qwerty123`). The model knows which tails are common (year, `!`, `123`, `!23`) and which templates dominate, and splits the budget across templates by their real frequency instead of letting one hot template flood the queue.
2. **pairs** — word+word joins (`iloveyou`), ordered by a bigram model: what word tends to follow what in real passwords. Same candidate space as classic PRINCE, but in probability order.
3. **chains** — three-word chains for long passphrases (beam search over bigram joins), reaching lengths that pairs cannot.

A **targeted mode** spins the generators around known words of a specific person or service: nicknames, names, dates (`--personal words.txt`), service names with automatic variants (`--service linkedin` adds `linkedin`, `linked`, `lnkd`, `mylinkedin`), and leet-normalization of the input (`--deleet`: `1v@n` counts as both `1v@n` and `ivan`).

Like PCFG and classic PRINCE, this generator trades speed for accuracy: it produces on the order of a million candidates per second per core, so it is best used against slow hashing algorithms (scrypt, PBKDF2 with high iterations, wallet formats) or large salted lists, where each guess is expensive and accuracy pays for itself. For fast hashes at full GPU speed, classic mask/rule attacks still win on throughput.

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

gcc, libm, no other dependencies. The C port is bitwise-verified against the Python reference (see `docs/c-port/`).

## Quick Start Guide

### With the prebuilt model

Download `sp-model-rockyou-v1.tar.gz` from Releases and unpack it next to the repo (it creates `data/`):

```
tar xzf sp-model-rockyou-v1.tar.gz
```

The default model was trained on the public RockYou dump, the same standard reference wordlist the original PRINCE ships with.

A short attack (struct head + pairs):

```
c/spg struct data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 1000000 > short.txt
c/spg pairs  data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 5000000 >> short.txt
hashcat -m 0 -a 0 hashes.txt short.txt
```

The full campaign (long attack, ~43M candidates):

```
c/spg struct  data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 14000000 >  campaign.txt
c/spg pairs   data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 25000000                                >> campaign.txt
c/spg chains  data/pw_uni.tsv data/pw_bi.tsv 10000  4000000 0.7 2000000 80                      >> campaign.txt
hashcat -m 0 -a 0 hashes.txt campaign.txt
```

Generation takes under a minute with `spg` (about 5 minutes with the Python version). All output is streamable — pipe straight into hashcat with `--stdin` if you do not want the file.

### Training on your own dump

Any password list works, one password per line:

```
python3 src/train_password_ngrams.py mydump.txt
```

This writes the model tables into `data/`. Both the Python and C generators pick up whatever is in `data/` — retraining is how you aim the tool at a specific language, community or leak.

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

Measured against classic PRINCE (pp64 with the same top-5000 dictionary, `--pw-max=32`, fixed config) on clean cross-dump test sets. "Clean" means: unique passwords of the dump, minus everything present in the training set, minus case-fold leakage (a candidate whose lower() form is in the training set is removed from the test). The RockYou row is validation-only (same-family data); the headline number is the cross-dump row.

| test set | @100K | @1M | @10M | final |
|---|---|---|---|---|
| linkedin (clean, 4.36M) | **x1.68** | **x2.70** | **x2.16** | 217,498 @ 43M vs pp64 104,797 @ 100M |
| rockyou (validation, 1.37M) | x2.13 | x2.28 | x2.30 | 154,574 @ 43M |

Full methodology, the exact pp64 command, raw logs and the red-team audits: `docs/METHODOLOGY.md` .

Known caveats, documented honestly:

- leet-heavy passwords are out of scope by design (measured: a leet layer adds under 1K hits) — use hashcat rules for that
- non-ASCII passwords (CJK, emoji) are emitted only if present in the training data
- the model cannot invent words it never saw: out-of-vocabulary names need `--personal`

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
