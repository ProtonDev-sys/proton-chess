# Human-style play: October 23, 2026

Proton 0.3.3 improves the existing opt-in `HumanStyle` selector and its move
preferences. These are handcrafted chess heuristics, not a model trained on
human games. This iteration makes no new Elo or measured human-likeness claim.

## Changes

- Prefer improvement from a piece's source square over absolute destination
  centrality. Development preferences depend on material and correctly typed
  minors on their home squares, rather than a FEN's move counter.
- Exempt captures, checks, check evasions, and safe retreats from quiet-tempo
  penalties. Encourage acquiring useful rook files and progressing endgame
  passers, excluding pawns blocked by another friendly pawn.
- Use provisional root scores for sampling, then enforce the loss allowance
  with a fresh restricted search at the completed depth. Remove the redundant
  narrow-window screening pass and its special transposition-table bypass.
- Specify weighted sampling using the top 53 RNG bits. Emit final UCI score and
  PV information when human selection changes the played move.
- Correct exchange evaluation when a promotion's apparent recapturer is pinned.
  In `Qn1R4/K1P5/1r1b4/8/3k4/8/8/8 w - - 0 1`, `a8b8` is +320 cp in SEE,
  rather than -130 cp. Tests cover both colours and the unpinned counterexample.

The historical baseline `3e9a1fc` reproduces the two policy assertions recorded
in the October 1 report. The zero-loss fixture now admits an independently equal
alternative. The cancellation fixture explicitly leaves one verification node;
it no longer assumes that a fixed whole-search cap will exhaust confirmation.
The existing successful node-limited confirmation integration test is retained.

## Local validation

Tests ran on the local machine using Ubuntu 24.04 under WSL, GCC 13.3,
Python 3.12, and chess 1.11.2. The supplied D: workspace had no free space, so
the checkout, builds, and raw records were kept on C:.

- Portable Release and native Release: all 13 CTest targets pass.
- Debug with ASan and UBSan, LTO disabled: all three C++ test targets pass.
- Perft regression suite passes. Independent differential perft passes on
  1,000 random positions at depth 2, seed 20261001. Legal-move cross-checks pass
  on 9 curated and 100 random positions, 20 plies, seed 17.
- Human selection check: first 50 positions from the bundled UHO suite,
  depth 4, 1 MB hash, book off, skill 20, maximum loss 12 cp, variety 100,
  seeds 1 through 32. All 1,600 choices repeat after resetting the seed and
  search state. All 31 selected alternatives pass an independent fresh
  restricted-search check against the full root search's 12 cp allowance.
  The actual UCI `bestmove` is checked, not the pre-selection thinking PV.
- Gameplay check: first six bundled match openings with colours reversed,
  human skills 20 and 12, variety 100, 1 MB hash, book off, seed 20261023,
  and 1,500 nodes per move. All 12 games finish legally: 11 checkmates and one
  threefold repetition, across 1,211 searched plies. Reported PVs agree with
  played moves and reported node counts remain within the limit.

Raw validation summaries and game records remain ignored under `results/`.
The small gameplay sample is a legality and integration check, not a strength
comparison. A completed depth's centipawn allowance is relative to this engine's
search, not a guarantee about deeper or external-engine evaluations.
