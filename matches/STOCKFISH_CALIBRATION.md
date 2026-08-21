# Fixed-node Stockfish calibration

`tools/calibrate_stockfish.py` runs one bounded calibration shard against an official Stockfish `UCI_Elo` level or full strength. Every opening is played twice with Proton's colors swapped. The shard runner is capped at 50 opening pairs so each process has a bounded lifetime.

Example shard:

```bash
python tools/calibrate_stockfish.py build/proton_chess /path/to/stockfish18 \
  --opponent-elo 1700 --pairs 50 --offset 0 \
  --nodes 1000 --seed 20260823 \
  --openings openings/uho_lichess_4852_v1_200.epd \
  --json out/stockfish18-1700-offset0.json
```

Run disjoint offsets such as `0`, `50`, `100`, and `150`, then merge them:

```bash
python tools/merge_stockfish_calibrations.py \
  out/stockfish18-1700-offset0.json \
  out/stockfish18-1700-offset50.json \
  out/stockfish18-1700-offset100.json \
  out/stockfish18-1700-offset150.json \
  --json out/stockfish18-1700-nodes1000.json
```

The merge tool rejects differing binary hashes, options, opening suites, node controls, source references, overlapping pairs, or overlapping openings. Its 95% lower score bound treats one color-swapped opening pair as the statistical unit.

A fixed-node calibration answers how the pinned engines compare under the same requested search-node budget. It is useful for locating a strength boundary and screening search changes, but it is not an absolute human Elo rating. Stockfish's `UCI_Elo` value is an advertised limiter setting whose result depends on the binaries, openings, node budget, adjudication, and hardware-independent protocol recorded in the report.

The separate `stockfish18_3000_60+0.6.json` protocol remains the long-time-control certification target. This fixed-node tooling does not modify its pinned runner or pass condition.

## Recorded calibration

`stockfish18_nodes1000_20260823_summary.json` records the current-main calibration against the official Stockfish 18 `sf_18` tag. Limited levels use 400 games from 200 unique color-swapped opening pairs; the direct full-strength result is a 100-game screen.

The record uses two explicit labels:

- **Confidently beats:** all 400 games are complete and the conservative 95% lower score bound is strictly above 50%.
- **Confidently dominates:** the confidently-beats condition is met and the observed score is at least 70%.

Under that protocol, current Proton confidently beats Stockfish 18 at `UCI_Elo=1700`, confidently dominates it at `UCI_Elo=1500`, does not establish a confident win at 1800, and is far below unrestricted Stockfish 18. These are properties of the pinned fixed-node experiment, not portable absolute Elo claims.

## Boundary extension

`stockfish18_nodes1000_boundary_20260823.json` adds 2,800 games around the limited-strength boundary, using the same 1,000-node control, 200 unique color-swapped opening pairs, seed, opening suite, and pair-based confidence rule.

The exact tested domination boundary is:

| Stockfish 18 `UCI_Elo` | Proton W-D-L | Score | Conservative 95% score interval | Result |
|---:|---:|---:|---:|---|
| 1567 | 253-84-63 | 73.750% | 64.147%-83.353% | confidently dominates |
| **1568** | **237-89-74** | **70.375%** | **60.772%-79.978%** | **confidently dominates** |
| 1569 | 225-99-76 | 68.625% | 59.022%-78.228% | confidently beats, but does not meet the 70% domination threshold |
| 1575 | 221-106-73 | 68.500% | 58.897%-78.103% | confidently beats, but does not dominate |
| 1750 | 156-119-125 | 53.875% | 44.272%-63.478% | inconclusive |

Therefore, `UCI_Elo=1568` is the highest tested integer Stockfish 18 setting that meets the repository's explicit **confidently dominates** definition. `UCI_Elo=1569` is the immediately adjacent tested setting and falls below the required 70% observed score. Proton still confidently beats the tested `UCI_Elo=1700` level, but levels 1701 through 1749 have not been exhaustively measured.

Stockfish's limiter is nonlinear and contains internal skill-depth transitions, so nearby `UCI_Elo` values are not expected to form a perfectly smooth score curve. None of these labels is a portable absolute human Elo estimate.
