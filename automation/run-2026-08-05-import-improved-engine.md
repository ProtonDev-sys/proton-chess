# Run Report: Import Improved Proton Chess

Date: 2026-08-05

Branch: `agent/import-improved-engine`

Source package: `proton-chess-improved-complete.zip`

## Scope

Replaced the existing working tree with the archive's single
`proton-chess-improved/` project root while preserving `.git`. The repository's
existing `.gitignore` and `.gitattributes` were retained because ZIP packaging
omitted dotfiles. Added a reproducible paired-opening Elo estimation tool and
documented its use.

## Build and correctness

Commands:

```text
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPROTON_NATIVE=OFF
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
python -m compileall python tools
python tools/benchmark_uci.py build/Release/proton_chess.exe --depth 11 --hash 64 --json
```

Results:

- Release build passed with MSVC 19.50.35729.0.
- One C4244 warning remains in `search.cpp` table initialization (`int` to
  `int16_t`); it does not fail the build or tests.
- CTest passed 2/2: the C++ suite (including six standard perft positions,
  randomized make/unmake and hash reconstruction) and the asynchronous UCI
  smoke suite.
- Python compilation passed for the included `tools` and `tests` scripts. The
  imported project has no `python/` directory, so the canonical command reports
  `Can't list 'python'` while returning success.
- The archive does not contain the legacy standalone `tools/check_perft.py` or
  `tools/crosscheck_movegen.py`; their core coverage is present in the bundled
  C++ tests, but the old commands themselves cannot be run.

Depth-11 benchmark:

| Position | Best move | Nodes | Time | NPS |
| --- | --- | ---: | ---: | ---: |
| startpos | d2d4 | 367,059 | 213 ms | 1,723,281 |
| kiwipete | d5e6 | 305,902 | 251 ms | 1,218,733 |
| endgame | b4c4 | 133,875 | 49 ms | 2,732,142 |

Release binary SHA-256:
`31303AA825CD1CB1D19D83FE482EB7EB24EE9C8DEE19BB465E6F5CAEE7AB2D94`

## Elo estimate

Reference engine: official Stockfish 18 Windows AVX2 build, SHA-256
`C86215FA1977D53B82ED854540A4C7B025BE4CD042276C85BA3DE53FB9118911`.

Confirmation command:

```text
python tools/estimate_elo.py build/Release/proton_chess.exe STOCKFISH \
  --opponent-elo 2200 --games 40 --move-time 0.08 \
  --max-plies 220 --hash 64 --seed 20260805
```

Method: 20 deterministic opening pairs, each played with colours reversed;
Proton's book and human mode disabled; Stockfish configured with one thread,
`UCI_LimitStrength=true`, and `UCI_Elo=2200`; both engines received 80 ms per
move and 64 MB hash.

Result: 28 wins, 5 draws, 7 losses (30.5/40, 76.25%). The logistic estimate is
2403 Elo with an approximate 95% interval of 2301-2553.

An earlier 60-game bracket at 30 ms/move produced non-monotonic results across
Stockfish's handicap levels, demonstrating that the setting is sensitive to
very short time controls. Therefore 2400 should be treated as a rough
engine-match estimate for the stated configuration, not as a FIDE, Chess.com,
or Lichess player rating.
