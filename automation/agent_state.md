# Proton Chess automation state

- Automation ID: `proton-human-strength`
- Goal: human-style, selectable-strength engine with a statistically significant match win over Stockfish at `UCI_Elo=3000`
- Last run: `2026-08-09T07:18:42+01:00`
- Status: `passed`
- Change kept: pinned Stockfish-3000 certification inputs, FEN starts, and raw-record result verification
- Active branch: `agent/pinned-stockfish3000-protocol`
- Base: `origin/main` at `2294ac8`

## Latest evidence

- The certification source is official-stockfish/books commit `65815cc`. The CC0 `UHO_Lichess_4852_v1.epd` archive, extracted EPD, SRI and position count were verified before derivation.
- The source contains 2,632,036 legal non-terminal positions. A fixed content-hash ranking selects 200 positions; a full 8m36 legality scan and the optimized 2.9-second candidate-first generator selected identical FENs.
- The checked-in suite SHA-256 is `4d94a44a0a3c8d687b145a4efd594acaa0ab8c904873784c662beafb50055b6d`. It has 200 unique legal positions: 110 white to move, 90 black to move, starting plies 5 through 16.
- The match runner accepts six-field FEN starts while retaining legacy UCI move-line openings. Pair identity includes the opening index, FEN and moves.
- The Windows AMD64 manifest fixes 400 games, 200 color-swapped pairs, Stockfish 18 at `UCI_Elo=3000`, `60+0.6`, one thread, 64 MB hash, seed `20260809`, a 600-ply cap, exact engine options and a strict pair-score lower-bound gate.
- Stockfish's official `stockfish-windows-x86-64.zip` SHA-256 is `40cc9758...81a139`; its executable and the locally used executable both hash to `9bde4202...6e8d6`.
- The launcher validates the full protocol shape, verifies platform, clean Git state, runner hash, exact `chess==1.11.2`, both engine hashes, suite hash and legal/unique position count, then launches private staged copies of every verified input.
- The checker replays exactly 400 raw game records, enforces the deterministic opening schedule and color swap, verifies clocks/terminations, and recomputes W/D/L, score, pair scores, pentanomial counts, Hoeffding bounds and the pass gate. Fabricated summaries, NaN values, wrong openings, incomplete pairs and dependency drift have focused regressions.
- A real FEN smoke exercised a black-to-move UHO position through both colors.
- A pre-final-manifest full-clock pair at `60+0.6` took 285.8 seconds and scored `0.5/2` for Proton: one checkmate loss and one threefold draw. Its one-pair interval is `[0, 1]`; it is not an Elo claim. The strict checker rejects it as 2/400, dirty-tree evidence and a pre-final suite-file hash.
- The focused tooling suites pass 48/48. Full canonical validation, 6/6 CTest targets, 5/5 perft cases and 49/49 move-generation positions pass.
- No engine-strength claim is made from this protocol run.

## Previous run: pair-aware inference

- Each color-swapped opening pair is one statistical sample. The two games within a pair are not treated as independent.
- Schema version 2 reports normalized pair scores, raw-score pentanomial counts, and a two-sided 95% Hoeffding interval over complete pairs.
- The report records the seeded without-replacement opening schedule, the finite-population bound assumption, residual shared-host caveat, and warns that the result is not a universal Elo rating.
- An incomplete one-game checkpoint reports zero complete pairs and the conservative `[0, 1]` interval rather than manufacturing evidence.
- A deliberately perfect 400-game/200-pair fixture produces a lower bound above 50% and clears the significance gate.
- JSON reports are written atomically after engine identification and every completed game, before corresponding console output. The final `complete` state is also persisted before the rendered report is printed. An interruption leaves the last intact checkpoint usable.
- Running checkpoints distinguish the all-completed-game score from the complete-pair score used for inference.
- Added ten focused inference, validation, callback, lifecycle-state, and atomic-write tests; the match-tool suite is now 19/19.
- Real two-game Stockfish-3000 Fischer smoke at `0.2+0.02` produced one drawn pair, pentanomial `{1.0: 1}`, interval `[0, 1]`, complete provenance, and a final `complete` checkpoint.
- Full canonical validation and 3/3 CTest targets passed.
- The smoke run is not a strength claim.

## Previous run: Fischer match lifecycle

- `tools/estimate_elo.py` accepts `--base-seconds 60 --increment 0.6` while retaining legacy fixed movetime.
- Each game uses fresh engine processes and a distinct token. `ucinewgame` and `isready` complete before the chess clock starts.
- Wall-clock time is deducted before increment. Exact zero and negative remaining time both flag; an increment cannot rescue a flagged move.
- Clock-only UCI searches run through an external watchdog. A stuck command is cancelled and its engine process is closed.
- The schema records engine IDs, hashes and options; opening/tool hashes; raw moves; opening indices; clocks; terminations; Git revision/dirty state; Python versions; and host details.
- Removed the invalid old unpaired confidence interval.
- Added nine focused clock, flag, timeout, schedule, and lifecycle tests.
- Full canonical validation and 3/3 CTest targets passed.
- This was match infrastructure, not a strength claim.

## Previous run: quiet-mate horizon

- Target FEN: `3r2k1/ppB2pp1/6b1/7p/1n1Pq3/2Q3P1/PP3P1P/2KR3R w - - 1 22`.
- Before the change, restricted `c3b4` at depth 1 scored `+617` in 2 nodes with PV `c3b4`; depth 2 found `c3b4 e4c2` as mate against Proton in 102 nodes.
- Quiescence now checks for an immediate legal quiet mate only within the first three plies. It returns the exact mate score and preserves the mating move in the TT and PV without adding general quiet-check recursion.
- After the change, the same restricted depth-1 search reports mate against Proton in 2 nodes with PV `c3b4 e4c2`.
- Exact pre/post depth-12 comparison kept identical node counts, scores, best moves, and PVs on all three fixtures:
  - startpos: `970427` nodes
  - Kiwipete: `551591` nodes
  - endgame: `253200` nodes
- Median aggregate benchmark time across three fresh processes was `1089 ms` before and `1076 ms` after. No slowdown claim is made from that small timing sample; it rules out the earlier broader scan's measurable overhead.
- A four-game paired fixed-movetime Stockfish-3000 smoke scored `0-4` both before and after. This is only a gross regression check; the current estimator cannot support a strength claim.
- Full canonical validation and CTest passed.
- No Elo improvement claim is made from the tactical correction.

## Previous run: UCI cancellation race

- Before the change, three independent eight-search transcripts containing `go infinite` followed immediately by `stop` all timed out.
- Root cause: the input thread could request a stop before the worker entered `Search::think`; the worker then cleared that request and searched indefinitely while the input thread blocked in `join()`.
- `Search::think` now offers an optional one-shot start callback immediately after its final stop-flag reset.
- The UCI layer waits for that callback before returning from `start_search`, so a following `stop` cannot be lost.
- The UCI smoke test now covers 16 immediate normal stops and four immediate ponder stops without waiting for search output first.
- After the change, the same three independent eight-search transcripts exited cleanly with 8/8 non-null best moves each.
- Final local validation passed:
  - `cmake -S . -B build`
  - `cmake --build build --config Release`
  - `ctest --test-dir build -C Release --output-on-failure`
  - `python -m compileall python tools`
  - `python tools/check_perft.py build/Release/proton_chess.exe`
  - `python tools/crosscheck_movegen.py build/Release/proton_chess.exe --count 40 --plies 10 --seed 17`
  - `python tools/benchmark_uci.py build/Release/proton_chess.exe --depth 10 --json`
- Search and evaluation behavior are unchanged; no Elo claim is made.

## Previous run: validation baseline

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

- Repair the current human move selector and UCI Elo mapping before extending the advertised 2800 ceiling.
- Add a shorter pinned paired protocol for iteration while reserving the `60+0.6`, 400-game run for clean release candidates.
- Keep future engine changes only with focused regressions, canonical validation, and paired before/after strength evidence.
