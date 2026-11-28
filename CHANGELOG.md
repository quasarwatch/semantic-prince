# Changelog

## v1.0.2 (2026-11-28)

Robustness pass over the whole tree, no interface changes and no quality change (the documented v8 campaign numbers reproduce exactly). Found by a full audit of the repo after the v1.0 release; every fix below was reproduced before and after under ASan/UBSan (C side) or against the recorded repro (Python side).

- Fix (trainers): both trainers now create the output directory themselves and fail fast with a clear message; previously a missing `data/` aborted the run after all training work was already done, with an error naming the output file as missing. Usage text and argument validation fixed to match reality
- Fix (genstruct): a failed filler-cache write (no `data/` next to `src/`, read-only directory) is now a stderr warning, not a crash with zero output; the model files can live anywhere
- Fix (evaluate): empty test set is a clean error instead of a ZeroDivisionError traceback; LIMIT <= 0 reports zero guesses instead of reading one candidate
- Fix (genpairs2/genstruct): an invalid mode string is rejected with the list of valid modes (a typo previously produced a valid-looking but wrongly ordered stream); `--no-dedup` and alpha are now parsed in any order after the positional arguments instead of silently dropping alpha
- Fix (spmodel): empty-vocabulary npz cache round-trip no longer corrupts K (0 came back as 1); uni/bi counts <= 0 or non-numeric are rejected with file:line
- Fix (C loaders): words_blob growth is a loop with an overflow guard (a word longer than K*12 bytes wrote past the buffer); the npz cache parser bounds-checks central-directory records; bi_i/bi_j, word lengths and counts from the npz cache are validated, a corrupt cache falls back to TSV with a warning instead of writing out of bounds
- Fix (C struct): dset_init no longer spins forever on LIMIT >= 2^62; candidate buffer sizing uses 64-bit arithmetic with an explicit cap (oversized digit/symbol run values previously wrapped it); emit() writes candidates larger than the 8 MB buffer directly instead of overflowing it; run/template counts out of range are rejected
- Fix (C chains): NaN/Inf scores are masked defensively (a zero/negative count in a TSV previously crashed chains while the Python side accepted the input)
- Fix (C targeted): i64set/i64map capacities are rounded up to a power of two (a non-power-of-two capacity degenerated probing into an infinite loop on otherwise valid input); NP above uint32 range is rejected; --deleet dedup now owns its copies (use-after-free/stack-use-after-scope on long or colliding deleet forms)
- Docs: README targeted/training examples all run out of the box; HARNESS.md and regressions.md updated to the current 41-case suite; gen_perf.py and dump_scores.py work when invoked from their own directory

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
