# Contributing

Issues and pull requests are welcome.

## Rules

- One problem per pull request.
- Any change to `src/` (Python) must be mirrored in `c/` (C), and the other way round. `cd c && make && ./tests/parity.sh` must pass with zero failures — CI enforces this on every PR.
- `make` must build with zero warnings (`-Wall -Wextra`).
- Never present output you did not run. Numbers in PRs come from real runs.
- Bug reports: include the exact command, the input (or a minimal version of it), the output, and what you expected instead.

## Style

- C: gnu99, Allman braces, 2-space indent, plain ASCII.
- Python: PEP8-ish, no new dependencies without a discussed reason (numpy only today).
