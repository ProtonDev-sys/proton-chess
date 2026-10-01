# Cleanup and unrestricted Stockfish checkpoint: October 1, 2026

## Status

**The unrestricted Stockfish 19 target is not achieved.** This report is a
research checkpoint, not a completion claim or a promise to beat Stockfish.
The retained engine is Proton 0.3.2, source `df98ae3`.

The latest official stable release was rechecked through the official GitHub
release API: `sf_19`, published September 5, 2026 at 08:33:17 UTC. The pinned
universal Windows binary has SHA256
`45bc8e4969147db9c2eb533810637994619bff0eacc81ccfd9854394901bcbd0`.

## Retained cleanup

- Remove the always-unavailable neural/GPU evaluator, its unused arguments and
  state, and inactive option storage. Rename the actual handcrafted evaluator
  to `StaticEvaluator`; do not imply that the engine contains a learned model.
- Preserve legacy inactive UCI option handling with explicit diagnostics, but
  do not advertise those options or flush useful caches for no-op settings.
- Remove the unused duplicate legal-capture generator.
- Use native BMI2 PEXT sliding-attack tables with a runtime capability check
  and the existing ray implementation as fallback. Portable builds retain rays.
  Exhaustively compare all 107,648 relevant blocker subsets, including edge
  occupancy, against an independent directional oracle.
- Reject nonchecking quiet moves before making them in the quiet-mate scan.
- Relocate ordinary pieces directly, updating board, piece/occupancy bitboards,
  and pawn hash once; retain separate promotion and capture handling. Extend
  random make/unmake tests to check reconstructed bitboards and occupancy.
- Propagate native architecture compile flags to consumers of inline attack
  functions, avoiding different GCC header implementations in different
  translation units.
- Fix claimed-draw PGN result headers and add draw, decisive-result, and
  unresolved-result regressions. A draw claim must not be exported as `*`.

Rejected trial code is not left in the production search or evaluator.
Frozen trial binaries and raw evidence remain ignored under `build/baseline/`
and `results/2026-10-01/optimization/`, not in the tracked source tree.

## Throughput and correctness

Final native executable SHA256:
`c08253bb2fe4e1e6cf3b69376d5087c121bdb5b77e8ab36f47c524e4db8aabc1`.
Final portable executable SHA256:
`6b29275dc28096d6b0f8e7f9385b41203a0a189fa8da9c33d1d04402424b5d41`.

The final native build versus the integrated 0.3.1 native baseline uses 50
positions, depth 10, three alternating fresh-process repetitions: **150/150
searches match exactly** in best move, ponder, score, depth, selective depth,
node count, and PV. Both engines are repeatable on all 50 positions. Candidate
engine time totals 32,910 ms versus 35,255 ms; measured total search wall time
is **6.537% lower**. This is a throughput measurement, not a measured Elo gain.
Some match workers overlap these measurements; do not treat timing as an
isolated-host hardware benchmark.

Additional comparisons each preserve 100/100 searches: final native versus
the strength-tested relocation snapshot, and final native versus final portable.
The latter measures 23.488% less native wall time in that run. Earlier 9.6%
screening results are not substituted for the final 6.5% measurement.

Both final Windows builds pass independent differential perft on 1,000 random
positions at depth 2 and 128 at depth 3. All 98 Python unit tests pass.
Native and portable CTest each pass 12 of 13 targets. The C++ engine target
still fails the two already-reproduced upstream assertions:

1. `a seeded rare Elo 3000 opportunity exercises a real alternative`
2. `exhausted confirmation falls back to the completed root best`

A local Ubuntu GCC native build independently reproduces those same two
failures, with the exhaustive attack checks passing. Tests are not weakened
or deleted to produce a green status. The complete suite is **not green**.

## Fresh matches

All matches use one thread and 64 MB hash per engine, opening books disabled,
1 ms move overhead, legal-terminal scoring, and color-swapped opening pairs.
Proton's strength limit and human selector are off. For unrestricted matches,
Stockfish explicitly has `UCI_LimitStrength=false` and `Skill Level=20`.

| Candidate / opponent | Games | ms/move | Seed | W/D/L | Score | 95% lower bound |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| Cleanup snapshot / unrestricted SF19 | 100 | 100 | 20261018 | 0/5/95 | 2.500% | 0.000% |
| Relocation snapshot / SF19 UCI_Elo 2200 | 200 | 100 | 20261021 | 137/25/38 | 74.750% | 62.511% |
| Relocation snapshot / unrestricted SF19 | 40 | 250 | 20261023 | 0/0/40 | 0.000% | 0.000% |

The fresh 2200 sample passes the predeclared lower-bound-above-50% gate, with
zero unresolved games. It is not a human Elo estimate or an unrestricted win.
Bounds use opening pairs, not individual games, with the same finite
50-opening suite sampled with replacement. Shared host load and Stockfish's
unseeded weakened-play randomness limit generalization. Different candidate
snapshots and time controls are not pooled into one confidence claim.

## Tried and reverted

All three trials use fixed 120-game self-play samples at 100 ms/move against
the same native baseline family, with no early stopping. None establishes a
strength gain under the conservative pair-based confidence gate.

| Trial | Change | Seed | W/D/L | Score | 95% lower bound |
| --- | --- | ---: | --- | ---: | ---: |
| Conservative LMR | Logarithmic reduction divisor 2.15 to 2.50 | 20261019 | 42/45/33 | 53.750% | 37.950% |
| Passer king proximity | Add rank-scaled endgame front-square king-distance term | 20261020 | 43/38/39 | 51.667% | 35.867% |
| Full-depth captures | Remove SEE-negative capture LMR | 20261022 | 47/39/34 | 55.417% | 39.617% |

The passer term was `(relative_rank - 2) * (5 * enemy_distance - 2 * own_distance)`
for relative ranks at least 3, using Chebyshev distances to the next pawn square.
It additionally breaks two existing evaluator checks. The capture trial
additionally fails an in-band human-selection fixture. Those changes remain
reverted; a small positive point estimate does not justify promotion.

## Evidence audit and next experiments

All **700 fresh games** are replayed as legal chess, their terminal outcomes,
candidate points and ply counts rechecked, and all six stored aggregate reports
recomputed exactly. There are no unresolved games in these completed samples.
Before the PGN-header fix, 105 claimed draws carry legacy `*` headers; their
JSON points and replayed outcomes are correct. Original evidence is preserved
rather than silently rewritten. The capture trial uses the repaired exporter.

Machine-readable provenance and summaries are in `optimization-2026-10-01.json`.
Raw per-game PGNs, JSON, build logs, and search-comparison records remain local
under `results/2026-10-01/optimization/`.

Prioritized research hypotheses, not achieved improvements:

1. Introduce independently trained, incrementally updated learned evaluation
   behind `Evaluator`, preserving existing chess-rule and protocol tests.
   Freeze datasets, model weights, inference cost, and unseen validation
   openings before strength comparisons; do not substitute Stockfish itself.
2. Trial incremental material/PSQT/phase accounting as an exact-score speed
   experiment. Compare make/unmake overhead against evaluation-loop savings
   before retaining it.
3. Trial staged move generation and ordering against the current quadratic
   selection loops, measuring both fixed-search behavior and real self-play.
4. Resolve the two existing policy fixtures without weakening independent
   loss-band or confirmation-budget invariants.
5. Require a separately frozen, sufficiently powered unrestricted SF19 match
   on unseen openings and more than one time control before declaring victory.
