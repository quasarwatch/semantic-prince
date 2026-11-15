# Notes for AI coding agents

What an agent needs that a human contributor already knows.

## What this project is

Semantic PRINCE generates password candidates in probability order, learned from real disclosed passwords. Two implementations live here:

- `src/` — the Python reference (trainer, generators, evaluator). This is the source of truth for behavior.
- `c/` — the C port (the `spg` binary). Any behavior change in `src/` must be mirrored in `c/` and pass the parity harness.

## Build and test

    cd c && make                    # builds ./spg
    ./tests/parity.sh               # C vs Python parity, exits 0 only when all cases pass

The parity harness compares C output against the Python reference on every subcommand: bitwise where deterministic, set+metric equality where tie-order may permute. Run it after any change to either side. `PY_ONLY=1 ./tests/parity.sh` checks the Python side against the goldens alone.

Never present output you did not run. Every number in a README, report or PR description must come from a command you actually ran and read back.

## Where the knowledge lives

- `docs/METHODOLOGY.md` — how the benchmarks were measured, what was rejected and why. Read it before touching scoring or ordering logic: several "obvious improvements" (leet layer, global template merge, year prioritization) are measured negative results.
- `src/` file headers — each generator documents its own algorithm at the top.

## Rules

- Keep the Python and C sides in lockstep; a fix that lands on only one side is a bug by definition.
- The 12 historical bugs live as regression cases in `c/tests/regressions.md` — do not weaken them.
- The model tables (`data/`) are not in git; they ship via Releases. Never commit dumps, model files or test sets.
- No emoji, no marketing tone in public text. Facts and reproducible commands only.
