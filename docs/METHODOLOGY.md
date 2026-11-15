# Methodology

How the benchmark numbers in the README were produced. Everything below was actually run; the raw logs live in the development history.

## Test sets

- **Training**: RockYou (public dump, 2009), 90% deterministic split by md5(line).
- **Validation**: the held-out 10% of RockYou (1.37M unique). Same-family data — used only for tuning, never as a headline.
- **Headline**: LinkedIn dump (4.36M unique passwords), which the model never saw during training.

## De-leakage

Every test set is cleaned twice:

1. **Exact overlap**: any password present in the training set is removed from the test set (no competitor scores for memorization).
2. **Case-fold overlap**: any test password whose lowercased form is in the training set is removed (a red-team pass found 37% of early hits were case variants of trained passwords — they are gone from the test set).

## Baseline

Classic PRINCE, one fixed configuration, same top-5000 dictionary for both sides:

```
pp64.bin --pw-max=32 < words5k.txt | head -n 100000000
```

## Measured results

| test set | @100K | @1M | @10M | final |
|---|---|---|---|---|
| linkedin (headline) | ours 4,643 / pp64 2,767 (**x1.68**) | 31,441 / 11,630 (**x2.70**) | 127,034 / 58,889 (**x2.16**) | ours 217,498 @ 43M vs pp64 104,797 @ 100M |
| rockyou (validation) | x2.13 | x2.28 | x2.30 | 154,574 @ 43M |

Additional context measurements (not headline claims):

- Cyclone_hk_v2 (608M-line breach compilation) beats everything at 100M on these dumps — because it contains the cracked answers to these same breaches (circular). At 100K guesses we beat it on both dumps (6,028 vs 3,911 on linkedin).
- pcfg_cracker was evaluated during development; on human-shaped passwords the word-anchored model wins at the budgets that matter.
- A pure case-mutation layer was measured and rejected: its entire win came from case-fold leakage into the test set (it went negative on clean sets).

## Rejected approaches (negative results, documented so nobody re-tries them blind)

- blanket leet-mutation layer over the stream (+976 hits max, not worth the budget)
- year-suffix prioritization (year-suffixed passwords are not under-served)
- keyboard-walk tokens (+6.3K hits, minor)
- global template merge by joint score (floods the head with digit templates; budget mode won)

## Reproducing

```
c/spg struct  data/pw_uni.tsv data/pw_bi.tsv data/struct_templates.tsv data/struct_digits.tsv data/struct_syms.tsv 10000 14000000 >  campaign.txt
c/spg pairs   data/pw_uni.tsv data/pw_bi.tsv 10000 pwbi 25000000                                >> campaign.txt
c/spg chains  data/pw_uni.tsv data/pw_bi.tsv 10000  4000000 0.7 2000000 80                      >> campaign.txt
c/spg evaluate <test-set.txt> < campaign.txt
```
