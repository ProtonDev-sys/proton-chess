# Proton Chess automation state

- Automation ID: `proton-human-strength`
- Goal: human-style, selectable-strength engine with a statistically significant match win over Stockfish at `UCI_Elo=3000`
- Last run: `2026-08-10T00:30:58+01:00`
- Status: `passed`
- Change kept: reproducible paired fixed-search comparison tooling
- Active branch: `agent/paired-search-compare`
- Base: `origin/main` at `44873f2`

## Latest evidence

- Added `tools/compare_search.py` for hash-pinned candidate-versus-baseline UCI screens at fixed depth or a fixed requested node cap. It stages private copies of both binaries and the opening suite, rejects identical binaries, records source and tool hashes, and re-verifies every input after the run.
- Each trial uses fresh engine processes with the same explicit full-strength options. Every FEN is preceded by `ucinewgame` and `isready`, then round-tripped through Proton's `d` command before timing. Candidate-first order flips by position and trial.
- Queue-backed output readers enforce real Windows deadlines without blocking on `readline`; stderr is drained through the same pipe. Failures and interruptions retain atomic JSON checkpoints with the active engine, trial and position. Cleanup attempts both engines even if the first close fails.
- Rows validate the exact requested depth, legal best move, legal reported PV, score type/bound, metrics, ponder move and fixed-node cap. Best move, ponder, score, depth, selective depth, reported nodes and PV must repeat across trials; timing and NPS are deliberately excluded from semantic equality.
- Fixed-node reports label `info nodes` as the last completed iteration rather than total budget consumption and omit node-efficiency deltas. Fixed-depth node comparisons carry an explicit unchanged-accounting caveat.
- The clean depth-8 run compared all 200 pinned UHO positions twice for 400 paired searches. Both binaries were internally deterministic. The current engine used 26,058,884 nodes versus 25,285,012 for pre-qsearch main (`+3.061%`); best moves matched 358/400, scores 270/400 and PVs 250/400. This confirms the known qsearch behavior difference and exercises the tool; it is not a new performance claim.
- The clean 25,000-node-cap run also completed 400 paired searches with repeatable results. Best moves matched 380/400, scores 336/400 and PVs 328/400. No reported completed iteration exceeded the requested cap, and the report publishes no node-saving percentage.
- The depth report SHA-256 is `cd80a6ded411d17628d04308d1e71484d66b8f3e38e3dac861ca04c1cac88f41`; the fixed-node report is `1779d24d4929522b847df0a4b394f524892331f09bae6c43925330ec91ca600e`. Both record clean revision `6b3467590f2810ed67b5910640aa936ab8729d97` and `git_dirty=false`.
- Thirteen focused regressions cover inputs, CLI bounds, malformed UCI output, FEN round trips, legal PVs, real timeouts, balanced ordering, repeatability, node semantics, atomic writes, failed checkpoints, cleanup and broken stdout pipes. Two independent final reviews found no blocker.
- Full validation passes: 8/8 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The comparator SHA-256 is `6f2250d1b95624e0a29295f15fba4e2b530ab04ed36fac7488da81b308ec8d1b`. The engine remains `55cee06290e1f1f92ae28869484a9e1f5111b9871e5af8b97e145c5c57b2875f`; the pinned certification runner remains `df6d62cf76bc3588b77177555c6ddd1234008aff49a5a1976308476bea9e26ee`.
- This run improves search-experiment reproducibility. It does not claim increased strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: quiescence checking captures

- Quiescence no longer discards a low-gain capture before determining whether it gives check. On `5bkb/7p/8/7Q/8/3B4/8/6K1 w - - 0 1`, old code returned the `+527` stand-pat under a deliberately high window; the candidate finds `Qxh7#` or `Bxh7#` at the exact `+31995` mate score from ply 4.
- `Position::gives_check` reconstructs post-move occupancy and friendly attacker sets without mutating the position. It covers ordinary captures, source-vacation discoveries, en passant's off-destination victim, promotions, castling, king moves and destination reblocking. `make_move` remains authoritative for own-king legality.
- Curated white/black en-passant, discovered-check, reblocking, promotion, castling and tactical fixtures plus 192 deterministic playout positions compare the prediction with real make/check/unmake results for every legal generated move. A direct qsearch regression proves a delta-prunable en-passant discovered check is searched.
- The first safe implementation made every otherwise-pruned capture and cost `+5.306%` nodes and `+9.380%` median engine time. The kept pre-move predicate exactly matches that correct search tree on all 200 UHO positions while reducing its median time by `1.51%` in separate balanced-order trials.
- Against merged main at depth 10, the kept candidate searched 55,926,703 versus 53,108,913 nodes (`+5.306%`). Median engine-reported time was 44,435 ms versus 42,870.5 ms (`+3.649%`). Best moves matched on 163/200 positions, scores on 66/200 and PVs on 53/200; the extra nodes are the cost of searching the newly preserved checking continuations.
- The completed 400-game, 200-pair A/B scored 143 wins, 109 draws and 148 losses: `49.375%`, approximately `-4.34` Elo. The paired result is inconclusive (`p_gain=0.6410`, `p_regression=0.4049`; score interval `[0.39772, 0.58978]`) and does not establish a gain, regression or non-inferiority.
- Final terminations were 291 checkmates, 76 threefold repetitions, 13 move-limit draws, 11 fifty-move draws and 9 insufficient-material draws. All 400 records, 200 pairs, totals and hashes recomputed with no illegal moves, flags, timeouts or protocol failures.
- One full attempt stopped after 347 games and one isolated-opening reproduction stopped after 6 without a diagnostic. The isolated opening then completed 20/20 under a traceback wrapper. A separate 60-game retry was interrupted by the agent turn rather than either engine. All partials are preserved and excluded; the same original seed completed under the wrapper on the final retry.
- The rebuilt validated executable has a different byte hash from the played candidate because production headers were recompiled after adding coverage and an API-contract comment. A fresh 200-position comparison matches the played candidate exactly on best move, score, depth, selective depth, nodes and PV.
- Three independent reviews found no checking-predicate or qsearch semantic defect. Their requested en-passant qsearch, ordinary discovery, destination-reblocking and API-contract coverage was added.
- Full validation passes: 7/7 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `55cee06290e1f1f92ae28869484a9e1f5111b9871e5af8b97e145c5c57b2875f`; the certification manifest pins it.
- This run keeps a tactical correctness repair. It does not claim statistically significant strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: rook support for passed pawns

- An own rook with a clear same-file ray behind its passed pawn receives a deliberately small `+8` middlegame / `+18` endgame bonus. Rooks in front, beside the pawn, behind an enemy passer or separated by any blocker receive none.
- Paired white/black evaluator fixtures isolate the 17 cp tapered increment in the test endgame and prove color and side-to-move symmetry. Front, blocked and enemy-rook negatives are covered.
- Across all 200 pinned UHO positions at depth 10 and Hash 16, the candidate used 53,108,913 nodes versus 53,009,557 for merged main (`+0.187%`). Best moves matched on 174/200 positions, scores on 125/200 and PVs on 124/200, confirming that the term is active with little search cost.
- A separate 20-game lifecycle smoke completed legally. The fixed 400-game, 200-pair A/B then scored 156 wins, 96 draws and 148 losses: `51.0%`, approximately `+6.95` Elo.
- The exact paired result is inconclusive (`p_gain=0.3281`, `p_regression=0.7165`; score interval `[0.41397, 0.60603]`). It does not establish a strength gain or non-inferiority.
- The final A/B had 304 checkmates, 76 threefold repetitions, 7 move-limit draws, 7 insufficient-material draws and 6 fifty-move draws. All 400 raw records, 200 color-swapped pairs, totals and binary hashes recomputed; there were no illegal moves, flags, timeouts or protocol failures.
- Independent evaluator review found no color/rank, blocker, duplicate-bonus, occupancy, double-counting or performance defect. Whole-evaluation constants in some negative fixtures are acknowledged as a maintenance cost.
- Two search experiments were rejected before this change: aspiration fail-high score carry increased nodes by `1.951%`; TT-refuted ProbCut saved only `0.0014%`. Neither remains in the tree.
- Full validation passes: 7/7 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `88d7bdfca50a50f6e21e0270165dd543bdd83f0f05ad8d6d97a272306dea27fb`; the certification manifest pins it.
- This run keeps a human-style evaluation improvement. It does not claim statistically significant strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: pawn-safe minor mobility

- Knight and bishop mobility now excludes destinations controlled by enemy pawns. Their complete pseudo-attack maps still feed king safety and other attack terms, and rook, queen and king mobility are unchanged.
- Mirrored evaluator fixtures prove one unsafe knight destination costs exactly 4 cp, one unsafe bishop destination costs 5 cp after tapering, both terms are color symmetric, and a rook scope guard is unchanged.
- Evaluation-sensitive limiter fixtures were retuned without weakening their invariants. A purpose-built two-move regression independently scores `Qb6` at `+46` and `b6` at `+10`, then proves the 36 cp-loss move is sampled and rejected against a 22 cp allowance by checking the following seeded RNG draw.
- The completed 400-game, 200-pair A/B against merged main scored 151 wins, 107 draws and 142 losses: `51.125%`, approximately `+7.82` Elo. The exact paired result is inconclusive (`p_gain=0.3232`, `p_regression=0.7168`); the screen detected neither a significant gain nor a significant regression and does not establish non-inferiority.
- The final A/B had 293 checkmates, 69 threefold draws, 16 insufficient-material draws, 14 move-limit draws and 8 fifty-move draws. It completed with no illegal moves, flags or timeouts.
- Two earlier attempts were excluded after a child process exited with code 1. The captured final position survived 100 fresh searches, five full 107-search state replays and five Linux ASan/UBSan replays; the traceback-enabled fixed sample then completed all 400 games. No engine defect was reproduced, so the interruptions are recorded rather than silently omitted.
- Independent audit found no color, sign, move-generation, attack-map or outpost double-counting defect. Pinned pawns intentionally remain part of the pawn-safe pseudo-mobility approximation.
- Full validation passes: 7/7 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `ada74c93ae05d7b39daf77954f4c2367a493c3c57427ba9aa39be762cc5f8bc5`; the certification manifest pins it.
- This run keeps a human-style evaluation improvement. It does not claim non-inferiority, increased strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: exact node caps and stop-aware re-search

- A sticky node/main-budget stop is now checked before every possible follow-up search: razoring continuation, deep null verification, ProbCut confirmation, LMR/PVS re-search and root PVS re-search.
- The child verifier receives the parent's exact remaining node budget. Any unexpected child overrun is rejected and the public parent count saturates at its cap rather than admitting a candidate or reporting an overrun.
- The focused restricted start-position search for `c2c4` at depth 8 now stops incomplete at exactly 4,000 nodes. The parent fixture begins at 100/4,100 nodes, spends the reserve and returns incomplete at exactly 4,100. Old code reports 4,001 and 4,101 respectively.
- A parent with zero remaining nodes declines confirmation without changing its counter.
- The existing full-strength 6,000-node fixture keeps `Qa7` at depth 5 but now reports exactly 6,000 rather than the historical 6,001. Elo 3000 still matches full strength.
- Across all 200 pinned UHO positions at 6,000 nodes, candidate and baseline matched 200/200 on completed best move, depth, score and PV.
- A symmetric 20-game timed smoke completed with no illegal moves, flags or timeouts. The candidate scored 8 wins, 3 draws and 9 losses (`47.5%`); the fixed-sample result is inconclusive and is not a strength claim.
- Independent audits verified every recursive continuation site and found no remaining stop-propagation gap or false-acceptance path.
- Full validation passes: 7/7 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `44d047da7d5bf143456aea86ab2d8b1e408f2b1ec6e54c8586847717e868775d`; the certification manifest pins it.
- This run fixes bounded-search correctness without changing completed fixed-node choices in the 200-position scan. It does not claim increased strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: symmetric candidate-versus-baseline runner

- Added `tools/compare_engines.py` for one candidate-versus-baseline comparison with candidate-relative records and no nominal opponent rating or fabricated absolute Elo.
- The runner stages and verifies private copies of both executables and the opening suite, then uses only those immutable copies. Source changes during staging and staged changes during play fail the run.
- Every game gets fresh candidate and baseline processes, identical explicit full-strength Proton options, the same deterministic per-game `HumanSeed`, and completed `ucinewgame`/`isready` handshakes before timing.
- Opening selection depends only on the configured seed. Each selected opening is played candidate-white then candidate-black and treated as one statistical sample.
- Atomic JSON begins in `running` state, checkpoints after every completed game, and records a final verdict only after the scheduled fixed sample completes. Running checkpoints cannot claim a gain or statistical significance.
- The independent schema records requested and even scheduled game counts, alpha, both labels/identities/paths/hashes, exact options, seed derivation, opening/tool/shared-core hashes, host, clocks, moves, pair scores, pentanomial counts, conservative score bounds, exact sign-flip probabilities and relative Elo delta.
- Candidate and baseline options must expose the same complete Proton control surface. A second-process launch failure closes the already-started first process.
- Ten focused tests cover configuration symmetry, profile rejection, exact seeds, relative inference, multi-opening scheduling, role order, process cleanup, private staging, running/final report lifecycle, hashes and invalid inputs.
- A real two-game smoke compared binary `1123e22a...c791e7` with `687e3175...78a28`. Both color-swapped games completed, both option maps matched exactly, and the final score was `1/2`; this is a lifecycle check, not strength evidence.
- Full validation passes: 7/7 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The A/B tool SHA-256 is `654d619fb83087ec4fcd098304065413d2d7b783b3e9e35194518fd3e9ac2a88`. The imported match core remains `df6d62cf76bc3588b77177555c6ddd1234008aff49a5a1976308476bea9e26ee`, exactly matching the pinned certification runner.
- The engine binary and certification manifest remain unchanged at `1123e22a742f70e18ba04012293f313ad17e62f70a4830ec91bd6b8e67c791e7`.
- This run improves benchmark reproducibility. It does not claim an engine-strength gain, calibrated Elo or a Stockfish-3000 win.

## Previous run: same-key transposition-table replacement

- Same-key TT writes now update move ordering independently from value replacement. A shallow non-exact result cannot discard a result more than two plies deeper; exact and near-depth results retain the previous behavior.
- Retained entries keep their score, static evaluation, depth and bound while refreshing their generation. A null move from a different-key bucket replacement now clears the evicted entry's unrelated move.
- Direct regressions cover deep-value preservation with both present and null incoming moves, independent move refresh, the two-ply boundary, shallow exact replacement, null-move preservation, generation refresh and different-key move clearing.
- Across all 200 pinned UHO positions at depth 9, candidate nodes fell by 0.21% at Hash 1 and 0.49% at Hash 64. Best moves matched the baseline on 193/200 and 194/200 positions respectively; the change is intentionally not described as behavior-preserving.
- A 20-game paired smoke completed legally at 50 ms/move with no engine errors. The candidate scored 7 wins, 4 draws and 9 losses.
- The 200-game screen scored 70 wins, 69 draws and 61 losses: 52.25%, approximately +15.65 Elo. The exact pair randomization result was inconclusive (`p_gain=0.1938`) at the strict `0.005` early-look threshold.
- The final 400-game, 200-pair run scored 145 wins, 111 draws and 144 losses: 50.125%, approximately +0.87 Elo. The exact test was inconclusive (`p_gain=0.5`, `p_regression=0.5511`) at the final `0.045` threshold. This is evidence of no detected regression, not evidence of an Elo gain.
- Full validation passes: 6/6 CTest targets, 5/5 perft cases and 49/49 move-generation positions.
- The Windows executable SHA-256 is `1123e22a742f70e18ba04012293f313ad17e62f70a4830ec91bd6b8e67c791e7`; the certification manifest pins it.
- The match runner's second-engine path did not set a deterministic `HumanSeed` on the baseline Proton binary. The paired matches are therefore only a gross regression screen, not strictly reproducible patch-isolation evidence.
- This run repairs TT replacement semantics and slightly reduces fixed-depth nodes. It does not claim increased strength, calibrated Elo or a Stockfish-3000 win.

## Previous run: reserved budget for bounded human play

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

- Add an explicit seeded alternative-selection opportunity rate so the 1200/1800/2400 modes can tune human-like error frequency independently from confirmed error size.
- Continue bounded full-strength search/evaluation work only when deterministic screens justify the match cost.
- Add singular extension only after explicitly tracking whether a TT move belongs to its stored value.
- Calibrate advertised Elo modes only after full-strength search and evaluation changes settle.
- Reserve the pinned `60+0.6`, 400-game Stockfish-3000 run for clean release candidates.
