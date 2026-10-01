# Integrated strength iteration: October 1, 2026

## Outcome

**Proton 0.3.1 passes the requested Stockfish 2200 difficulty milestone at both
tested time controls.** This does not establish a human Elo rating, strength at
longer time controls, or victory over unrestricted Stockfish.

The opponent is the latest official stable release verified on October 1:
Stockfish 19 (`sf_19`, published September 5, 2026). Its universal Windows binary
selects AVX512ICL on this machine. Both engines use one thread, 64 MB hash,
1 ms move overhead, and disabled opening books; Proton's human selector and
strength limit are disabled. Stockfish is explicitly strength-limited for Elo
tests and explicitly unlimited for the unrestricted test.

| Fresh 2200 confirmation | Games | W/D/L | Score | 97.5% one-sided lower bound |
| --- | ---: | --- | ---: | ---: |
| 100 ms/move, seed 20261013 | 400 | 254/73/73 | 72.625% | 63.022% |
| 250 ms/move, seed 20261014 | 200 | 141/24/35 | 76.500% | 62.919% |

Both fixed samples complete with zero unresolved games. Opening pairs are the
statistical units, not individual games. Hoeffding bounds on independently
sampled pairs give at least 95% joint coverage for these two lower bounds by
the union bound. Both exceed the predeclared 50% score gate. Earlier screenings
and the pre-integration candidate's confirmations are not pooled into them.
The candidate and sample sizes remain frozen; no early stopping is used.

This gate applies to two fresh samples from the same finite 50-opening suite,
sampled with replacement and color-swapped. It is not an unseen-opening or
all-time-control guarantee. Stockfish's weakened-play randomness is not seeded
by the opening seed. Shared host load affects timing and weakened choices.

## Integration and experiments

The initial local branch was 52 commits behind remote main. Merge `02a2131`
preserves upstream `5442524` and local experiment history rather than overwriting
newer work. The frozen tested native binary is `build/baseline/merged-native.exe`:

```text
SHA256 9f3de1ead664acedcbac8c0f910927c74f3a643d5392862c40d75e4e1c475b23
```

- Retain conservative singular extensions: independently exclude a deep TT move
  to test whether alternatives fail low before extending that move. Verification
  cannot borrow a full-position TT cutoff or overwrite that node's full-position
  entry. Eligibility, check interactions, and cancellation are bounded.
- Reject the experimental mobility, coordinated king-ring pressure, and dynamic
  passer variants during integration: they break established evaluation/policy
  regressions. The integrated evaluator matches upstream exactly.
- Preserve upstream's existing stale-TT-move replacement fix and add a direct
  replacement regression; do not retain a redundant second fix.
- Add fresh-state paired matches, explicit engine options and binary hashes,
  conservative confidence bounds, protected PGN output, atomic JSON progress,
  legal-terminal scoring, differential perft, and real benchmark timeouts.
- Preserve the existing calibration/protocol tools rather than replacing their
  established interfaces. Download the official Stockfish release with asset
  digest verification and selective extraction.

An equal-native-build self-play test against exact upstream, 160 games at
100 ms/move (seed 20261017), scores **52/58/50, or 50.625%**, with a 95% one-sided
lower bound of **36.942%**. This is inconclusive: the singular-extension experiment
has not demonstrated a strength gain. The milestone result is not evidence that
any individual change improved Elo. Earlier ablations and rejected variants
remain documented in `strength-2026-10-01.md` as historical evidence only.

## Higher-level gap

Exploratory ladder: 40 games per setting, 100 ms/move, two workers, seed 20261015.
Unrestricted Stockfish uses a separate 40-game sample, seed 20261016.

| Stockfish setting | W/D/L | Score | 95% one-sided lower bound |
| --- | --- | ---: | ---: |
| UCI_Elo 1600 | 39/1/0 | 98.750% | 71.383% |
| UCI_Elo 2000 | 33/3/4 | 86.250% | 58.883% |
| UCI_Elo 2400 | 23/9/8 | 68.750% | 41.383% |
| UCI_Elo 2800 | 2/16/22 | 25.000% | 0.000% |
| Unrestricted | 0/3/37 | 3.750% | 0.000% |

2400 looks promising but does not pass the confidence gate. 2800 and unrestricted
Stockfish remain substantially stronger. Beating Stockfish at every setting is
not achieved, and no such claim is made.

## Validation and known failures

Portable and native builds each pass **12 of 13 CTest targets**. The C++ engine
target has two failures: `a seeded rare Elo 3000 opportunity exercises a real
alternative` and `exhausted confirmation falls back to the completed root best`.
Both reproduce when independently building and testing the **unmodified upstream
source** with this Windows compiler. They are not new integration failures;
neither assertion is weakened or deleted. The suite is not reported as green.

New TT/excluded-search regression assertions pass. UCI, human-policy, threat-eval,
match protocol/calibration/tool tests, and differential-perft CTest targets pass.
Additional python-chess differential perft passes 1,000 random positions at depth
2 (seed 20261001) and 128 at depth 3 (seed 20261002).

All **960 integrated-candidate games** have their embedded PGNs replayed for legal
moves and final-position scores, and pair completeness and confidence summaries
independently recomputed. Together with the previously audited 1,460 historical
games, the iteration includes **2,420 audited games** across different candidates.
Only the integrated candidate's fresh 600 confirmation games support its gate.

Five alternating portable/native depth-11 benchmark repetitions run after all
matches finish. Moves, scores, depths, and node counts match between these builds.

| Position | Nodes | Portable median | Native median | Speed ratio |
| --- | ---: | ---: | ---: | ---: |
| Start position | 493804 | 309 ms | 269 ms | 1.149x |
| Kiwipete | 548701 | 476 ms | 421 ms | 1.131x |
| Endgame | 156264 | 60 ms | 54 ms | 1.111x |

These measure compilation speed differences, not algorithmic playing-strength
gains. Native builds require AVX2; the default remains portable.

## Evidence and cleanup

`strength-integrated-2026-10-01.json` records binary identities, applied options,
opening hash, all run settings, exact summaries, validation, and benchmark medians.
Full JSON, PGN, and logs remain local under `results/2026-10-01/merged-*`, ignored
by Git. Match PGNs are protected from accidental overwrites. Reproduce with a
fresh output filename, for example:

```powershell
python tools/paired_match.py build/baseline/merged-native.exe external/sf_19/stockfish-windows-x86-64-universal.exe --opponent-elo 2200 --games 400 --move-time 0.1 --workers 6 --seed 20261013 --confidence 0.975 --json results/repeat-2200.json --pgn results/repeat-2200.pgn
```

Generated dependencies, builds, downloads, and match output stay untracked.
Duplicate downloads and temporary source/build trees are moved to
`D:/Development/Proton Chess Recovery/2026-10-01/` rather than deleted because
recursive deletion is blocked. Baseline binaries, source history, useful books,
active builds, and evidence are preserved. Local and recovered copies do not
automatically synchronize.

## Next experiments

1. Ablate singular extensions against this exact upstream at equal native builds
   and larger fixed self-play samples; keep the present result inconclusive.
2. Investigate the two Windows-reproducing policy assertions in a separate focused
   correctness change before treating the entire test suite as green.
3. Profile move ordering and attack generation, then test bounded pruning changes
   with tactical, draw-rule, and fixed-node regressions plus fresh match samples.
4. Explore incrementally updated learned evaluation with licensed training data
   and independently checked inference; do not claim NNUE strength before testing.
5. Add endgame-tablebase probes and focused pawn-ending/zugzwang conversion tests.
6. Freeze a subsequent candidate and preregister fresh 2400 confirmations at both
   time controls, keeping losses against higher and unrestricted settings visible.
