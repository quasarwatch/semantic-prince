# Methodology

How the benchmark numbers in the README were produced. Everything below was actually run; nothing is estimated.

## Test sets

- **Training**: RockYou (public dump, 2009), 90% deterministic split by md5(line). The v2 model is *account-weighted*: each training password is counted as many times as it appeared in the dump (usage counts from the public rockyou-withcount list), so the model head reflects account frequency. v1 used an unweighted model.
- **Validation**: the held-out 10% of RockYou (1.37M unique). Same-family data — used only for tuning, never as a headline.
- **Headline**: LinkedIn dump, which the model never saw during training. Used in two forms:
  - *clean* (4.36M unique): minus training-set overlap, minus case-fold overlap — measures pure generalization;
  - *raw* (5.09M unique): the dump as shipped — measures real-world stacks.

## De-leakage

The clean test sets are cleaned twice:

1. **Exact overlap**: any password present in the training set is removed from the test set (no competitor scores for memorization).
2. **Case-fold overlap**: any test password whose lowercased form is in the training set is removed (a red-team pass found 37% of early hits were case variants of trained passwords — they are gone from the test set).

Known-plaintext replay heads are built the same way: the training-side passwords in dump-frequency order, never touching test entries. Note the consequence: on a *clean* test set a replay head scores exactly zero by construction — replay value only exists on raw targets.

## Baseline

Classic PRINCE, one fixed configuration, same top-5000 dictionary for both sides:

```
pp64.bin --pw-max=32 < words5k.txt | head -n 100000000
```

## Measured results (model-only, clean sets, v2 weighted model)

| test set | @100K | @1M | @10M | final |
|---|---|---|---|---|
| linkedin (headline) | 4,470 / 2,767 (**x1.62**) | 35,743 / 11,630 (**x3.07**) | 91,840 / 58,889 (**x1.56**) | **194,986 @ 43M** vs 104,797 @ 100M |
| rockyou (validation) | 4,718 / 2,101 (**x2.25**) | 29,149 / 9,010 (**x3.24**) | 66,960 / 41,596 (**x1.61**) | **143,950 @ 43M** vs 72,484 @ 100M |

Each cell: Semantic PRINCE / pp64 cracked-count and ratio. pp64 column reproduced verbatim from the current artifacts (2,767 / 11,630 / 58,889 / 104,797 on clean linkedin).

### v1 (unweighted) vs v2 (account-weighted) model

Same 43M campaign recipe on both models, clean sets:

| test set | | @100K | @1M | @10M | final |
|---|---|---|---|---|---|
| linkedin | v1 | 4,643 | 31,440 | 89,363 | 199,577 |
| | v2 | 4,470 | 35,743 | 91,840 | 194,986 |
| rockyou (val) | v1 | 4,763 | 22,720 | 63,984 | 140,086 |
| | v2 | 4,718 | 29,149 | 66,960 | 143,950 |

v2 wins the 1M zone decisively (+14% linkedin, +28% rockyou) and is within a few percent at the head and at exhaustion; on raw dumps it wins everywhere (+2-6% at 100K, +20-26% at 1M). v2 is the shipped default.

### Real-world stack (raw dumps, unique-cracked)

LinkedIn raw (5.09M unique). Both stacks spend the first 12.9M guesses on the same replay head (training-side passwords in dump-frequency order), so the curves split only after the dictionary exhausts:

| total guesses | rockyou + pp64 | rockyou + Semantic PRINCE |
|---|---|---|
| 100K | 66,869 | 66,869 |
| 1M | 304,930 | 304,930 |
| 10M | 565,429 | 565,429 |
| ~54M | 705,256 | **803,674** (+14.0%) |

Model-only on the same raw dump: 41,006 vs 8,602 at 100K (x4.8); 420,917 @ 43M vs 202,971 @ 100M final (x2.07 at 43% of the budget).

Webhost000 raw (1.65M unique): model-only 86,028 @ 43M vs 28,228 @ 100M (x3.05); stack final 156,730 (9.51%).

### Account-weighted view

Weighting each cracked password by its account count (rockyou-withcount), the 43M campaign on rockyou validation covers 11.5% of accounts at 100K guesses, 18.6% at 1M, 21.5% at 10M and 34.1% at the end — versus 0.52% / 2.56% / 5.34% / 11.10% of unique passwords. The stream concentrates on high-multiplicity passwords early. Account-weighted cross-dump numbers are not reported: usage counts for linkedin/webhost000 are not available to us.

### Additional context measurements (not headline claims)

- Cyclone_hk_v2 (608M-line breach compilation) beats everything at 100M on these dumps — because it contains the cracked answers to these same breaches (circular). At 100K guesses we beat it on both dumps (6,028 vs 3,911 on linkedin).
- pcfg_cracker was evaluated during development; on human-shaped passwords the word-anchored model wins at the budgets that matter.
- A pure case-mutation layer was measured and rejected: its entire win came from case-fold leakage into the test set (it went negative on clean sets).

## Anomalies and open items

- **SecLists 000webhost.txt (639K)** behaves non-standardly for every generator we tested (pp64 scores near zero on it, compilation lists score abnormally high). We excluded it from comparisons; treat any result on that file with suspicion.
- On clean linkedin at the 100K mark the v2 model scores marginally below v1 (4,470 vs 4,643) — the weighting trades a little head variety for mid-zone mass. Documented, accepted.

## Rejected approaches (negative results, documented so nobody re-tries them blind)

- blanket leet-mutation layer over the stream (+976 hits max, not worth the budget)
- year-suffix prioritization (year-suffixed passwords are not under-served)
- keyboard-walk tokens (+6.3K hits, minor)
- global template merge by joint score (floods the head with digit templates; budget mode won)
- temperature calibration of digit scores (fixes the head, breaks the tail)

## Reproducing

```
tar xzf sp-model-rockyou-v2.tar.gz    # from Releases; creates data/
c/spg struct  data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 14000000 >  campaign.txt
c/spg pairs   data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 25000000                                >> campaign.txt
c/spg chains  data/pw_uni.tsv data/pw_bi.tsv 10000  4000000 0.7 2000000 80                      >> campaign.txt
c/spg evaluate <test-set.txt> < campaign.txt
```

The exact candidate files of the measured campaigns are reproducible byte-for-byte from the v2 data pack and the commands above (C port is deterministic; parity-verified against the Python reference on 41 cases).
