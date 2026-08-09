# Proton Chess automation state

- Automation ID: `proton-human-strength`
- Goal: human-style, selectable-strength engine with a statistically significant match win over Stockfish at `UCI_Elo=3000`
- Last run: `2026-08-09T05:02:10+01:00`
- Status: `passed`
- Change kept: restored the canonical validation surface and cross-platform CI
- Active branch: `agent/reproducible-strength-baseline`
- Base: `origin/main` at `282f012`

## Latest evidence

- The 0.2.0 engine built successfully before the change and its two CTest tests passed.
- The canonical perft check initially failed because the validator had been removed and the engine now emits `info string perft depth ... nodes ...`.
- The canonical move-generation cross-check initially failed because the `moves` debug command had been removed; every tested position returned an empty engine move list.
- Restored `tools/check_perft.py`, `tools/crosscheck_movegen.py`, their curated positions, and the `python` tooling package.
- Restored the `moves` debug command without changing search or evaluation behavior.
- Added a Windows/Linux GitHub Actions validation workflow.
- Final local validation passed:
  - `cmake -S . -B build`
  - `cmake --build build --config Release`
  - `ctest --test-dir build -C Release --output-on-failure`
  - `python -m compileall python tools`
  - `python tools/check_perft.py build/Release/proton_chess.exe`
  - `python tools/crosscheck_movegen.py build/Release/proton_chess.exe --count 40 --plies 10 --seed 17`
- Move-generation evidence: 49/49 positions matched python-chess, including 9 curated edge cases.
- No performance or strength claim is made from this validation-only change.

## Repository routing

- The original checkout contains pre-existing uncommitted legacy work and remains untouched.
- Active development uses a clean linked worktree so that work cannot be lost during the 0.2.0 architecture transition.

## Next target

- Establish the pinned Stockfish-3000 match protocol and a trustworthy baseline on 0.2.0.
- Audit the current human move selector and UCI Elo mapping before extending the advertised 2800 ceiling.
- Keep future engine changes only with focused regressions, canonical validation, and paired before/after strength evidence.
