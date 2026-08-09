# Match protocol

`stockfish18_3000_60+0.6.json` is the Windows AMD64 certification protocol. It fixes both engine binaries, the runner, chess library version, opening suite, clock, pairing, engine options, seed, game count and pass condition used for the Stockfish 3000 claim.

Run it from the repository root:

```powershell
python tools/run_match_protocol.py matches/stockfish18_3000_60+0.6.json build/Release/proton_chess.exe C:\path\to\stockfish-windows-x86-64.exe --json out/stockfish18_3000_60+0.6.json
```

The launcher checks the platform, Proton and Stockfish binaries, runner, chess library version and every legal unique opening before starting. The match runner then records the binary hashes, tool hash, host, clocks, moves and per-game results in the output report.

The result passes only when all 400 games are complete and the pair-aware 95% score bound is strictly above 50%. Short runs are useful for checking the machinery, but they are not evidence for the strength claim.

Check a finished report with:

```powershell
python tools/check_match_result.py matches/stockfish18_3000_60+0.6.json out/stockfish18_3000_60+0.6.json
```

The checker rejects changed clocks, hashes, options, seeds, incomplete pairs, short matches and lower bounds at or below 50%.
