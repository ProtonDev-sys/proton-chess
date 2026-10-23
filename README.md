# Proton Chess

A standalone C++20 UCI chess engine. Version 0.3.3 uses handcrafted tapered
evaluation and single-threaded alpha-beta search: PVS, aspiration windows,
transposition tables, quiescence, SEE, null-move pruning, late-move reductions,
history heuristics, pawn correction history, and conservative singular extensions.
Evaluation includes pawn-safe minor-piece mobility, king safety, supported
passers, and bounded cheaper-attacker threats. This is not an NNUE or GPU engine.

## Build and test

Run from the repository root with CMake 3.20+, a C++20 compiler, and Python 3.10+:

```powershell
python -m pip install -r requirements.txt
cmake -S . -B build
cmake --build build --config Release --parallel 4
ctest --test-dir build -C Release --output-on-failure
```

The Windows executable is `build/Release/proton_chess.exe`. For single-config
Linux builds, configure with `-DCMAKE_BUILD_TYPE=Release`; the executable is
`build/proton_chess`. Python-chess enables match-tool and differential-perft tests;
the C++ tests and UCI smoke test also run without it.

For local strength tests, an optional optimized build is useful:

```powershell
cmake -S . -B build/native -DPROTON_NATIVE=ON
cmake --build build/native --config Release --parallel 4
ctest --test-dir build/native -C Release --output-on-failure
```

MSVC native builds require AVX2. GCC/Clang native builds target the build machine
and may not run on other CPUs. The default build remains portable. For GCC/Clang
debugging, `-DPROTON_SANITIZERS=ON -DPROTON_LTO=OFF` enables ASan and UBSan.

Native release builds use occupancy-indexed sliding attacks when compiled with
BMI2 intrinsics and the running CPU reports BMI2 support. Otherwise the existing
portable ray implementation is used. Both paths are checked against an independent
directional reference over every relevant occupancy subset. Neural/GPU placeholder
classes and unused option storage are removed; legacy unsupported UCI option
names still receive an explicit inactive/not-implemented response.

## Play

Add the executable to a UCI-compatible chess GUI. Run from this directory to use
the bundled opening book, or set `BookFile` to its absolute path. `UseBook` and
`HumanStyle` can be disabled for deterministic analysis. Only one search thread
is supported; increasing `Threads` is not implemented. Proton's own `UCI_Elo`
setting is a difficulty control, not a validated human rating.

## Reproducible strength testing

The downloader checks the official GitHub release archive's size and SHA-256
digest, retaining only the executable, license, attribution, and release metadata:

```powershell
python tools/fetch_stockfish.py
python tools/fetch_stockfish.py --tag sf_19
```

The first command selects the latest stable release; pin a tag to reproduce an
experiment. Stockfish 19 was the latest stable release verified for the October
1, 2026 iteration, published September 5, 2026. Downloads are ignored by Git.

Example paired match on Windows:

```powershell
python tools/paired_match.py build/native/Release/proton_chess.exe external/sf_19/stockfish-windows-x86-64-universal.exe --opponent-elo 2200 --games 400 --move-time 0.1 --workers 6 --seed 22001001 --confidence 0.975 --json results/2200.json --pgn results/2200.pgn
```

Repeat `--opponent-elo` for a difficulty ladder; omit it to test the opponent at
full strength. The second executable can also be an older Proton build for
self-play. The existing `tools/estimate_elo.py` and match/search protocols remain
available with their own command-line interfaces and validation suites; they are
not replaced by this additional confirmation runner. On Linux, use the paths
printed by the downloader and the single-config build path.

Each randomly sampled opening is played with both colors. Both engines receive
one thread, equal hash, 1 ms move overhead where supported, books off, and maximum
candidate strength. Results include binary hashes, applied options, settings,
PGNs, and partial progress. Existing PGN files are refused to prevent mixing
runs. Use unique output filenames for each experiment. Illegal moves or engine
errors fail the run; nonterminal ply-capped games are unresolved, never draws.

Confidence bounds use bounded opening-pair scores, not independent individual
games. A pass requires the one-sided lower bound above 50% and no unresolved
games. The bounds assume independent opening pairs sampled with replacement;
they do not cover arbitrary positions, different hardware/time controls, or
human ratings. Engines share the host while multiple workers run, so record and
keep the worker count and background workload comparable. Stockfish's weakened
play has its own randomness, which the opening seed does not control.

Freeze the candidate and sample sizes before confirmation. Screening runs guide
development but are not a substitute for fresh confirmation; do not pool selected
successful runs or stop early when a running score looks good.

## Diagnostics

```powershell
python tools/benchmark_uci.py build/Release/proton_chess.exe --depth 11 --json
python tools/benchmark_uci.py build/Release/proton_chess.exe --nodes 1000000 --json
python tests/differential_perft.py build/Release/proton_chess.exe --positions 1000 --depth 2
```

Benchmarks enforce a real output deadline. Compare repeated measurements on an
otherwise idle machine; fixed-node or fixed-depth timing is not a strength test.

Builds, downloads, caches, and full match output stay outside tracked source.
Keep baseline binaries and experiment records as local recovery/evidence rather
than committing generated artifacts. The measured iteration report belongs in
`docs/strength-integrated-2026-10-01.md`. Historical pre-integration experiments
remain in `docs/strength-2026-10-01.md`; they are not the current binary's results.

## Human-style play

Use the engine through any UCI-compatible chess interface. The direct controls are:

```text
setoption name HumanStyle value true
setoption name HumanSkill value 16
setoption name HumanMaxLossCp value 12
setoption name HumanVariety value 60
setoption name BookRandomness value 70
setoption name HumanSeed value 1
```

The UCI default targets a strong club player's style: sound plans with believable
mistakes. This is a behavioral profile, not a measured 1800–2200 rating.
`HumanSkill` ranges from 0 to 20. Lower values widen the set of acceptable
alternatives. `HumanMaxLossCp` sets the intentional centipawn-loss ceiling at
skill 20; the default skill 16 allows up to 76 cp against the completed root
search. `HumanVariety` is the percentage chance that a position can consider a
verified alternative. `HumanSeed` makes opportunity decisions and choices
reproducible. `BookRandomness` controls the variety of repertoire preferences.

For full-strength analysis, disable both `HumanStyle` and `UseBook`.
Explicitly disabling `HumanStyle` takes effect even if the GUI retains a lower
`HumanSkill`. `UCI_LimitStrength`, when enabled, selects its own difficulty
profile. The C++ `EngineOptions` defaults retain full-strength search; the UCI
interface uses `club_play_options()`.

For standard UCI strength limiting, use:

```text
setoption name UCI_LimitStrength value true
setoption name UCI_Elo value 2200
```

The supported UCI Elo range is 800–3000. The Elo control maps to the same bounded human selector rather than merely cutting search depth. Its profiles independently taper the acceptable loss and alternative-selection opportunity rate; these modes are behavioral presets, not certified rating guarantees.

Human mode does not blindly add noise. A failed opportunity roll returns the full-search move without reserving confirmation budget. An active opportunity preserves forced mates and confirms a sampled candidate with a separate restricted search before returning it. Root search bounds guide sampling; only a completed confirmation can admit an alternative inside the configured loss allowance. An interrupted confirmation returns the completed root best.

Meaningful intentional losses (over 25 cp) must also look reasonable at depth
two, within 35 cp of that iteration's best score. This favors mistakes revealed
by deeper calculation over obvious missed material. Repetition is discouraged
when an equal or better verified continuation exists; drawing repetitions are
preserved when continuing would worsen the searched score.

A compact native move prior learns preferences from rated 1800–2200 Lichess
rapid and classical games. Its influence is bounded, with no runtime downloads
or Python dependency. Opening repertoire weights also guide human candidates
through the same confirmation search. The training protocol and held-out move
prediction results are described in the iteration report below.

The style policy favours improving piece placement, finishing development,
castling, useful rook files, and coherent endgame support or blockade of advanced
passed pawns. Checks,
check evasions, and safe retreats from attack are exempt from quiet-development
penalties. Development depends on the pieces on the board rather than the FEN's
move counter. Seeded opportunity decisions and weighted move choices use defined
sampling rules across standard-library implementations.

## Validation and benchmarking

The repository includes native tests, UCI protocol smoke tests, perft regressions, legal move-generation cross-checks, deterministic fixed-search comparison tooling, and paired engine-match tooling under `tools/`.

A passing test suite proves correctness of the covered invariants; it is not by itself an Elo claim. Strength changes should be evaluated with the pinned paired-search and colour-swapped match protocols described in `matches/README.md`.

The current human-play changes and local validation are recorded in
[`docs/human-play-2026-10-23.md`](docs/human-play-2026-10-23.md).
The historical cleanup checkpoint is `docs/optimization-2026-10-01.md`: exact-search throughput improves, a fresh Stockfish 19 UCI_Elo 2200 sample passes, and unrestricted Stockfish 19 remains decisively stronger. Its two policy-test failures are addressed in 0.3.3; the original report is retained as the record of that build.
