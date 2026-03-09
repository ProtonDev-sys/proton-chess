# Proton Chess

Proton Chess is an experimental open-source chess engine written in C++20 with supporting Python tooling for testing, data generation, and future neural-evaluation work. The current codebase is a classical search engine with a generated opening-book workflow and a small training scaffold for later NNUE-style and deep-eval integration.

## What is in this repo

- A UCI engine with legal move generation, perft support, iterative deepening alpha-beta, quiescence, aspiration windows, a transposition table, null-move pruning, and LMR.
- A baseline evaluator plus GPU-ready deep-eval interfaces.
- Match, watcher, and play-against-engine scripts.
- A generated opening-book pipeline built from official Lichess broadcast PGNs.
- Training schema and export stubs for future neural models.

## Current strength

This is still an early engine. It is competitive against weaker engines and lower strength-limited Stockfish settings, but it is not yet close to unrestricted top engines.

Current verified checkpoints:

- `python tools/check_perft.py build\proton_chess.exe`
- `python tools/crosscheck_movegen.py build\proton_chess.exe --count 40 --plies 10 --seed 17`
- Sunfish, `8` games, `0.2s/move`, plain starts: `3 wins / 1 loss / 4 draws`
- Stockfish 18 with `UCI_Elo=1800`, `8` games, `0.2s/move`: `5 wins / 1 loss / 2 draws`
- Stockfish 18 with `UCI_Elo=1900`, `8` games, `0.2s/move`, `mini_suite`: `5 wins / 2 losses / 1 draw`
- Stockfish 18 with `UCI_Elo=2000-2100`: competitive in some short samples, but still unstable across reruns
- Stockfish 18 unrestricted and Koivisto 9.0: still losing

## Build

```powershell
cmake -S . -B build
cmake --build build --config Release
```

## Run

```powershell
.\build\proton_chess.exe
```

Supported engine commands:

- `uci`
- `isready`
- `ucinewgame`
- `position startpos moves ...`
- `position fen <fen> moves ...`
- `go depth <n>`
- `go movetime <ms>`
- `go wtime <ms> btime <ms> winc <ms> binc <ms>`
- `perft <depth>`
- `d`

## Opening book generation

The engine can load weighted UCI opening lines from `openings/book_lines.txt`. To regenerate that file from official Lichess broadcast archives:

```powershell
python -m pip install zstandard
python tools\build_opening_book.py `
  --download-url https://database.lichess.org/broadcast/lichess_db_broadcast_2026-02.pgn.zst `
  --download-url https://database.lichess.org/broadcast/lichess_db_broadcast_2026-01.pgn.zst `
  --download-url https://database.lichess.org/broadcast/lichess_db_broadcast_2025-12.pgn.zst `
  --max-games 30000 `
  --max-ply 18 `
  --root-branching 16 `
  --branching 6 `
  --min-count 4 `
  --min-ratio 0.02
```

Downloaded `.zst` source files are cached under `openings/data/` and ignored by Git.

## Tooling

Syntax check:

```powershell
python -m compileall python tools
```

Run a small match against Sunfish:

```powershell
python tools\run_match.py build\proton_chess.exe tools\sunfish_uci.cmd --games 8 --seconds 0.2
```

Run against strength-limited Stockfish:

```powershell
python tools\run_match.py build\proton_chess.exe external\bin\stockfish18\stockfish\stockfish-windows-x86-64.exe --games 8 --seconds 0.2 --opponent-option UCI_LimitStrength=true --opponent-option UCI_Elo=1800
```

Watch a live match:

```powershell
.\watch_stockfish.bat
```

Play against Proton locally:

```powershell
python tools\play_against_engine.py build\proton_chess.exe --human-color white --seconds 0.2 --pgn .\out\human_vs_proton.pgn
```

## Repo layout

- `src/`: engine source
- `python/`: training and export scaffolding
- `tools/`: perft, movegen cross-check, and match runners
- `openings/`: opening suite and generated book output
- `configs/`: evaluation harness configs

## License

This repository is licensed under the MIT License. See `LICENSE`.
