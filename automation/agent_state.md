# Proton Chess Agent State

- Last run: 2026-08-05
- Branch: `agent/import-improved-engine`
- Imported package: `proton-chess-improved-complete.zip`
- Project version: 0.2.0
- Status: validated and ready for publication
- Release binary: `build/Release/proton_chess.exe`
- Release SHA-256: `31303AA825CD1CB1D19D83FE482EB7EB24EE9C8DEE19BB465E6F5CAEE7AB2D94`
- CTest: 2/2 passed (`proton-tests`, `proton-uci-smoke`)
- Deterministic benchmark: 806,836 nodes across three depth-11 positions
- Elo confirmation: 30.5/40 against Stockfish 18 at UCI_Elo 2200 and 80 ms/move
- Estimated engine-match Elo: 2403 (approximate 95% interval 2301-2553)
- Known warning: MSVC C4244 narrowing conversion from `int` to `int16_t` in search-table initialization
- Validation note: the imported package replaces the legacy standalone
  `check_perft.py` and `crosscheck_movegen.py` tools with bundled C++ perft,
  randomized make/unmake/hash, legality, and UCI regression tests.

See `automation/run-2026-08-05-import-improved-engine.md` for evidence.
