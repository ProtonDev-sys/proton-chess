# Proton Chess automation state

- Automation ID: `proton-human-strength`
- Goal: human-style, selectable-strength engine with a statistically significant match win over Stockfish at `UCI_Elo=3000`
- Last run: `2026-08-09T17:45:24+01:00`
- Status: `passed`
- Change kept: reserved confirmation budget for bounded limited play
- Active branch: `agent/limiter-budget-reservation`
- Base: `origin/main` at `1a39eb2`

## Latest evidence

- Positive-loss limiter modes now separate main-search budget exhaustion from real stop/cancel state. The main phase reserves half the node cap or bounded hard time for one fresh same-depth candidate confirmation.
- Bounded selection samples once from PVS-plausible moves. No alternative is returned unless the isolated verifier completes at the same depth and scores inside the configured loss allowance; every interruption falls back to the completed root best.
- External stop sources are carried as a propagated list, caller deadlines remain sticky, and a bounded ponder that finishes before ponderhit returns the root best instead of starting an unbounded child verifier.
- Zero-loss Elo 3000 and full-strength searches do not reserve. On the 6,000-node regression both return `Qa7` at depth 5 with the historical 6,001-node accounting result.
- At Elo 800 with a 3,450-node cap, seed 27 now spends more than half the budget, completes depth 3, and returns a real confirmed alternative without exceeding the parent cap. A two-node search still returns a legal move within its cap.
- On `rnbqkbnr/pp1ppppp/8/8/2N5/3P4/PPP1PPPP/R1BQKBNR b KQkq - 0 3`, Elo 2700 seed 32 at depth 6 selects `g8f6` with a 22,000-node budget. At 7,500 nodes the verifier exhausts its reserve and deterministically falls back to root best `d7d5` within the total cap.
- Final node scans over the first 50 UHO positions and five seeds found 208/250 Elo-800 alternatives with maximum fresh loss 642/700 cp, and 10/250 Elo-2700 alternatives with maximum loss 22/22 cp. There were no violations.
- At 50 ms over the first 30 positions and five seeds, Elo 800 selected 118/150 alternatives and Elo 2700 selected 7/150, again with zero loss-band violations. Median wall times were 33.72 ms and 28.02 ms; the 95th percentiles were 50.28 ms and 43.10 ms.
- Full strength and Elo 3000 matched exactly on best move, completed main depth, main nodes, score and PV across the first 100 UHO positions at 6,000 nodes.
- Full validation passes: 6/6 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `687e31758ba3f4a31a2a0bc74eedee68548adeca602fa7aa500b221a11578a28`; the certification manifest pins it.
- This run makes bounded limiting effective and fail-closed. It does not claim calibrated Elo or increased full strength.

## Previous run: Elo 3000 control and calibration tooling

- `UCI_Elo` now advertises `default 2800 min 800 max 3000`. Every integer setting through 2800 retains the old skill/loss profile. From 2801 to 3000, skill remains 20 and the base loss allowance decreases monotonically from 8 cp to 0 cp; only 3000 reaches zero.
- The Elo-3000 regression is non-vacuous. On `rnb1kb1r/ppp1pppp/5q2/8/8/3PBQ2/PPP2KPP/RN3B1R w kq - 2 9` at depth 3, seed 2 chooses `f3f6` instead of root best `b1c3`; a fresh restricted search scores the alternative `-55` against the root score `-84`.
- The match runner accepts `--proton-elo`, derives a distinct deterministic `HumanSeed` from the protocol seed, target, effective opponent, pair and color, and enables `UCI_LimitStrength` last. Full 64-bit seeds are recorded as decimal strings.
- Stockfish's advertised Elo range is inspected before play. A requested 1200 calibration opponent is honestly recorded as requested 1200, effective 1320; values above Stockfish's maximum and duplicate effective levels are rejected.
- Proton's requested target is rejected rather than clamped when it falls outside the engine's advertised 800..3000 range.
- Full-strength certification keeps its exact Proton option set and never adds `--proton-elo`. The result checker accepts legacy schema-2 reports but rejects contradictory target, seed, requested/effective Elo and range metadata when those fields are present.
- Focused tests pass: 28/28 match-runner tests and 16/16 certification-checker tests. Native profile tests cover every Elo through 2800, monotonicity through 3000, endpoint clamping, and a confirmed zero-loss alternative.
- Full validation passes: 6/6 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- A real two-game configuration smoke ran Proton target 1200 against effective Stockfish 1320 at 0.02 seconds per move. It exercised both colors, exact per-game seed recording and requested/effective labels. Its 2/2 result is not calibration evidence.
- The Windows executable SHA-256 is `d90878c62b8c83b4d5610189428eef49e3e94eae2de0d3cfc4e99feef7d5eca9`; the runner SHA-256 is `df6d62cf76bc3588b77177555c6ddd1234008aff49a5a1976308476bea9e26ee`. The certification manifest pins both.
- This run establishes controls and reproducibility. It does not claim calibrated Elo or increased full strength.

## Previous run: human limiter correctness

- `UCI_LimitStrength` and `UCI_Elo` now have independent state. Setting either no longer overwrites custom human-style options; `HumanSkill` and `Skill Level` no longer toggle a separate Boolean.
- Replaying every advertised UCI default used to change startpos depth-4 play from `b1c3` to `g1f3`. Fresh defaults, replayed defaults, a `HumanSkill` 0-to-20 round trip and disabled `UCI_Elo=800` now all return `b1c3` with seed 27.
- Non-best candidates first pass a TT-independent narrow filter. A sampled alternative is returned only after a fresh isolated 1 MB search completes the same depth with `searchmoves` and confirms `score >= best - allowance`. The verifier watches both the parent stop and the caller's stop, uses the earlier parent/caller deadline, and shares the remaining node budget.
- On `3rk3/ppp2ppp/8/8/3Q4/8/PPP2PPP/4K3 w - - 0 1`, the old Elo-800 selector chose `a3`: independent depth-4 scores were `+677` for `Qa7` and `-595` for `a3`, a 1,272 cp loss against a 700 cp allowance. Seed 27 now chooses `Qe4`, independently scored `+659`, an 18 cp loss.
- Five pinned seeds all choose an independently verified move within the 700 cp band. A 3,450-node interruption reaches the limit and falls back to a verified in-band move.
- A tighter Elo-2700 regression previously approved a 52 cp loss against a 22 cp allowance. Fresh confirmation rejects that move and returns the best line.
- Limited timed play skips the full-strength opening-book shortcut. Full-strength book behavior is unchanged.
- Hash-1 and Hash-64 audits each checked 500 Elo-2700 decisions across 100 positions and five seeds: 45 alternatives, zero violations, and a worst accepted loss exactly on the 22 cp boundary.
- Across 100 positions with zero move overhead, 10 ms searches topped at 10.88 ms and 50 ms searches at 50.99 ms. Sampled fixed-depth median overhead was 1.35x to 1.52x through depth 10; the worst case was 3.51x.
- The new Windows executable SHA-256 is `65b416d42a1e6b634af15513db443714209b34ceed1ca7bf6a0cdb2e2cae6a0a`; the certification manifest pins it.
- Full canonical validation passes: 6/6 CTest targets, 5/5 perft cases, and 49/49 move-generation positions.
- This run fixes limiter correctness. It does not claim calibrated Elo or increased full strength.

## Previous run: pinned certification protocol

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
