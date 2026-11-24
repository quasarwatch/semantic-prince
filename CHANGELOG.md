# Changelog

## v1.0.1 (2026-11-24)

Model and robustness update, no interface changes.

- The prebuilt model in Releases is now `sp-model-rockyou-v2.tar.gz`: account-weighted training (passwords weighted by real usage counts). Model-only gains vs v1 on raw cross-dump sets: +2-6% at 100K guesses, +20-26% at 1M; on clean sets v2 wins the 1M zone (+14-28%) and is within a few percent elsewhere (full tables in docs/METHODOLOGY.md)
- Recommended campaign vocabulary is now K=20000 (was 10000): a wash below 1M guesses, +3-5% at 43M. The tables in the data pack cover 68K words; K is a runtime parameter, no re-download needed
- Fix (pairs/chains): after dedup, a top-up round could silently skip an equal-score candidate when tie order permuted between rounds; top-up now rescans under the exact dedup set. No quality change, ordering semantics now exact
- Fix (targeted): the "reachable" metric counts unique passwords, not split registrations; duplicate test lines no longer inflate the slice
- Fix (struct): NTPL < 1 is rejected instead of silently loading all templates
- Fix (pairs/chains): limit <= 0 exits immediately instead of sorting the full matrix
- Parity suite: 41 cases (34 bitwise + 2 canonical + 4 exit-code + 1 skipped selftest), 0 failures
- Repository hygiene: compiled artifacts and test workdir removed from git
- The data pack now includes `count_1w.txt`, which the trainer needs — retraining from the published artifacts works as documented

Note on the v1.0 notes: "4-10x faster" was optimistic; measured C-vs-Python speedups are 2.4-3.9x depending on the layer (pairs 25M: 54.9s -> 14.1s).

## v1.0 (2026-11-15)

First public release.

- struct/pairs/chains generators ordered by an n-gram model trained on real password leaks
- targeted mode: --personal / --service / --deleet / --emit
- C port (spg): bitwise-verified parity with the Python reference
- parity test harness covering every subcommand plus the full regression suite
- prebuilt RockYou model in Releases
