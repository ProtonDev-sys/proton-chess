# Proton Chess

Proton Chess is a single-threaded C++20 UCI chess engine with a classical evaluator and a selective alpha-beta search. This build focuses on legal correctness, practical playing strength, stable time-control behaviour, and optional human-like root move selection.

It is a native Proton implementation, not a Stockfish fork. It does not contain Stockfish source code or an NNUE network.

## Build

A portable release build:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPROTON_NATIVE=OFF
cmake --build build --config Release -j
ctest --test-dir build -C Release --output-on-failure
```

A build optimized for the current CPU:

```bash
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release -DPROTON_NATIVE=ON
cmake --build build-native --config Release -j
```

Do not distribute a `PROTON_NATIVE=ON` binary to machines that may lack the same CPU instruction set.

AddressSanitizer and UndefinedBehaviorSanitizer validation on GCC or Clang:

```bash
cmake -S . -B build-sanitize \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPROTON_SANITIZERS=ON \
  -DPROTON_LTO=OFF
cmake --build build-sanitize -j
ctest --test-dir build-sanitize --output-on-failure
```

The executable is `proton_chess` (`proton_chess.exe` on Windows). The packaged Linux build is under `bin/linux-x86_64/`.

## UCI configuration

Maximum native strength:

```text
setoption name Hash value 256
setoption name UseBook value true
setoption name BookRandomness value 0
setoption name HumanStyle value false
setoption name Skill Level value 20
```

Strong human-like play:

```text
setoption name Hash value 256
setoption name UseBook value true
setoption name BookRandomness value 10
setoption name HumanStyle value true
setoption name HumanSkill value 20
setoption name HumanMaxLossCp value 8
setoption name HumanSeed value 0
```

Human mode does not add noise inside the search. It verifies plausible root alternatives, rejects moves outside the configured loss band, strongly favours the best move at high skill, and does not randomize away a proven mate. `HumanSeed=0` uses a nondeterministic seed; a non-zero value makes root choices reproducible.

`UCI_Elo` and `UCI_LimitStrength` map to Proton's skill controls for GUI compatibility. They have not been independently calibrated to FIDE, online, or computer-engine ratings and should not be treated as measured Elo.

The weighted text opening book is used for timed games only. Fixed-depth, node-limited, `infinite`, and `searchmoves` searches analyze the position instead of returning a book move.

## Engine features

The board implementation uses incremental Zobrist and pawn keys, piece and occupancy bitboards, canonical en-passant hashing, precomputed attack rays, reusable move buffers, and complete make/unmake state restoration.

The evaluator tapers middlegame and endgame scores and includes material, piece-square placement, mobility, pawn structure, passed pawns, king safety, bishop pair, outposts, rook activity, and low-material conversion terms. Pawn and full-position evaluation caches avoid repeated work.

The search includes iterative deepening, aspiration windows, principal-variation search, clustered transposition tables, mate-score normalization, transposition reuse in quiescence, static exchange evaluation, adaptive null-move pruning with verification, mate-distance pruning, razoring, reverse futility pruning, late-move pruning, logarithmic late-move reductions, conservative ProbCut, killer/history/counter-move ordering, capture history, continuation history, correction history, and root policy ordering.

UCI search runs asynchronously and supports `stop`, `ponder`/`ponderhit`, clocks and increments, `movetime`, node limits, `infinite`, `mate`, and `searchmoves`. Interrupted iterative-deepening searches return the move from the last fully completed iteration rather than a partially overwritten root result.

## Validation

The test suite covers six standard perft positions, randomized make/unmake and hash reconstruction, pawn keys, castling, en passant, promotions, repetition and null moves, dead-material cases, attack tables, static exchange evaluation, rule-50 mate precedence, quiescence stalemate, human-mode mate preservation, restricted root searches, malformed UCI input, asynchronous stopping, and ponder handling.

A deterministic search benchmark is available:

```bash
python3 tools/benchmark_uci.py build/proton_chess --depth 11 --hash 64
```

A paired-opening match can give a rough, time-control-specific Elo estimate
against Stockfish's limited-strength settings (requires `python-chess`):

```bash
python3 tools/estimate_elo.py build/proton_chess /path/to/stockfish \
  --opponent-elo 2200 --games 40 --move-time 0.08
```

This is an engine-match estimate, not a FIDE or online-player rating. The tool
plays every sampled opening with colours reversed and reports a 95% interval.

## Current limits

Proton remains a handcrafted, single-threaded engine. It has no NNUE evaluator, Lazy-SMP search, Syzygy tablebase probing, direct legal move generator, or large-scale Fishtest-style SPRT tuning. These are substantial gaps relative to unrestricted top engines.

## License

MIT. See `LICENSE`.
