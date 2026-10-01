# Match protocol

`stockfish18_3000_60+0.6.json` is the Windows AMD64 certification protocol. It fixes both engine binaries, the runner, chess library version, opening suite, clock, pairing, engine options, seed, game count and pass condition used for the Stockfish 3000 claim.

Run it from the repository root:

```powershell
python tools/run_match_protocol.py matches/stockfish18_3000_60+0.6.json build/Release/proton_chess.exe C:\path\to\stockfish-windows-x86-64.exe --json out/stockfish18_3000_60+0.6.json
```

The launcher checks the platform, Proton and Stockfish binaries, runner, chess library version and every legal unique opening before starting. It copies the verified inputs into a private short-lived staging directory and launches only those copies. The match runner then records the binary hashes, tool hash, host, clocks, moves and per-game results in the output report.

The result passes only when all 400 games are complete and the pair-aware 95% score bound is strictly above 50%. Short runs are useful for checking the machinery, but they are not evidence for the strength claim.

Check a finished report with:

```powershell
python tools/check_match_result.py matches/stockfish18_3000_60+0.6.json out/stockfish18_3000_60+0.6.json
```

The checker rejects changed clocks, hashes, options, seeds, incomplete pairs, short matches and lower bounds at or below 50%.

## Candidate versus baseline

Use the separate A/B runner to compare two Proton builds. It stages verified private copies, starts fresh processes for every game, applies the same explicit full-strength options and per-game seed to both binaries, reuses each opening with colors swapped, and reports score from the candidate's point of view.

```powershell
python tools/compare_engines.py build/Release/proton_chess.exe C:\path\to\baseline.exe --games 400 --move-time 0.05 --max-plies 240 --hash 16 --seed 20260809 --openings openings/uho_lichess_4852_v1_200.epd --json build/bench/candidate-vs-baseline.json
```

The report contains both binary hashes, both UCI identities, the exact option map, opening and tool hashes, every move, pair scores, pentanomial counts, a conservative score interval, an estimated relative Elo delta and an exact paired sign-flip result. It does not assign either engine an absolute Elo.

This tool is deliberately separate from the pinned Stockfish certification runner. Changes to A/B reporting cannot silently change the certification protocol or its runner hash.

## Deterministic search comparison

Use `compare_search.py` before spending time on games. It runs the same positions through two Proton binaries with fresh processes on every trial, clears search state before every FEN, alternates which engine goes first and rejects a run if either binary changes its answer between trials.

For a fixed-depth tree comparison:

```powershell
python tools/compare_search.py build/Release/proton_chess.exe C:\path\to\baseline.exe --candidate-label candidate --baseline-label main --openings openings/uho_lichess_4852_v1_200.epd --depth 10 --hash 16 --trials 4 --timeout 120 --json build/bench/candidate-depth10.json
```

The report pins both binaries, the opening file and the tool by SHA-256. It records best move, score, completed depth, selective depth, nodes, engine time and the reported principal variation for every search. Node totals from fixed-depth runs are suitable for comparing deterministic tree size when both revisions use the same node-counting boundaries. Engine and wall times are host-specific supporting measurements.

Fixed-node mode answers a different question: what line does each engine finish under the same requested node cap?

```powershell
python tools/compare_search.py build/Release/proton_chess.exe C:\path\to\baseline.exe --openings openings/uho_lichess_4852_v1_200.epd --nodes 25000 --hash 16 --trials 2 --timeout 60 --json build/bench/candidate-nodes25000.json
```

Proton prints `info nodes` after completed iterative-deepening passes. If a node cap interrupts the next pass, the last reported count is not the total consumed against the cap. The tool therefore records that count for repeatability but deliberately omits node-efficiency deltas in fixed-node mode.
