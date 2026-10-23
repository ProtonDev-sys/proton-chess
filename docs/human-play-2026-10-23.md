# Human-style play: October 23, 2026

Proton 0.3.3 targets a strong club player's style: sound plans with plausible
mistakes. The UCI interface defaults to `HumanStyle=true`, `HumanSkill=16`,
`HumanVariety=60`, and `BookRandomness=70`. This is a behavioral target, not a
measured 1800–2200 rating or a claim of human indistinguishability.

## Move selection

An active human opportunity samples candidates using search scores,
handcrafted preferences, a compact learned move prior, and legal opening-book
weights. The learned prior's relative influence is capped at 32 times; a book
preference contributes at most 16 times the sampling odds. The best three
learned moves and repertoire continuations can enter consideration beyond the
ordinary search-rank prefix. Neither source bypasses search confirmation.

Every selected alternative requires a fresh restricted iterative search at the
completed root depth. The default loss ceiling is 76 cp: the skill-20 base of
12 cp plus `8 * (20 - skill) + 2 * (20 - skill)^2`. An intentional loss over
25 cp must also remain within 35 cp of the completed depth-two best score.
This uses the confirmation search's existing shallow iteration, favoring
mistakes that become apparent through deeper calculation over obvious material
misses. It adds no second verification search.

Repeated positions receive lower sampling weights. When the incumbent repeats,
a verified alternative must preserve or improve its searched score. This keeps
drawing repetition defenses available. Forced mates, `searchmoves`, stop
requests, node limits, and deadlines retain their protections; an interrupted
confirmation returns the completed root best. Final UCI information describes
the actual selected move and its score/PV.

A failed opportunity roll preserves full-budget analysis without book-prior
or learned-prior effects. Explicitly disabling `HumanStyle` restores analysis
even when a GUI retains a lower skill value. Disable `UseBook` as well for
deterministic analysis; `UCI_LimitStrength` selects its own profile when enabled.
The C++ `EngineOptions` defaults retain full-strength search; the UCI interface
uses the explicit `club_play_options()` factory.

## Learned human move prior

The 53-feature conditional logistic model was fitted to recorded moves from
rated 1800–2200 Lichess rapid/classical games. Its features describe captures,
checks, attacks and defenses, development, activity, pawn progress, rook files,
and endgame king movement. Inference is compiled C++ with no external model,
Python dependency, GPU, or runtime download.

The experiment uses a fixed 32 MiB compressed prefix of Lichess's September
2026 monthly standard-game archive, published under CC0. It excludes BOT-title
games, the first 12 plies, and decisions with insufficient recorded clock.
Sampling retains at most 12 decisions per game. The 27,001 positions from 2,276
games split into 18,731 training, 4,137 validation, and 4,133 test positions by
game. Hyperparameters and baseline score scaling use validation only.

| Prior over all legal moves | Test move match | Test NLL, nats |
| --- | ---: | ---: |
| Uniform | 6.30% | 3.288 |
| Captures/checks | 18.93% | 3.056 |
| Handcrafted policy | 21.10% | 3.092 |
| Learned prior | 31.97% | 2.407 |

Move match averages uniformly over tied maximum scores. The learned-minus-
handcrafted difference is 10.88 percentage points, with a game-bootstrap 95%
interval of 9.58–12.19 points. Only eight test states overlap training or
validation; excluding them gives 31.96% versus 21.08%. The handcrafted baseline
is the frozen evaluator policy identified in the model manifest, before the
additional pawn-plan changes below.

These results measure the standalone prior, not final engine move accuracy.
The contiguous sample comes from one day and is mostly rapid; players overlap
between splits, although games do not. The 504 decisions with both players
unseen also favor the learned prior (29.96% versus 18.72%), but form a small
subgroup. Safety features may explain much of the gain. Search constraints,
book weights, and bounded mixing change the final choice distribution.

Full provenance, weights, hashes, and descriptive results are in
[`models/human-club-v1.json`](../models/human-club-v1.json). Reproduction commands
and data limitations are in
[`tools/human_policy/README.md`](../tools/human_policy/README.md). Raw games and
training artifacts remain ignored.

## Contextual preferences and correctness

- Reward improvement from the source square. Development follows material and
  correctly typed home minors, rather than a FEN's move counter. Captures,
  checks, evasions, and safe retreats escape quiet-development penalties.
- Encourage acquiring useful rook files and progressing true endgame passers.
  In queenless endings, a stable pawn target connects king approach and clear
  rook support behind a passer. Stopping an advanced enemy passer takes
  priority. Rear doubled pawns do not become separate targets.
- Use specified RNG sampling rather than implementation-dependent standard
  distributions. Identical seeds reproduce choices under identical searches;
  time-limited searches and different hardware can complete different depths.
- Correct SEE when a promotion's apparent recapturer is pinned. In
  `Qn1R4/K1P5/1r1b4/8/3k4/8/8/8 w - - 0 1`, `a8b8` is +320 cp, rather than
  -130 cp. Tests cover both colors and the unpinned counterexample.
- Remove the unused move-history accessor/storage, which was never populated.
  Repetition preferences use the working position-key history.

## Validation

Local testing uses Ubuntu 24.04 under WSL, GCC 13.3, Python 3.12, and chess
1.11.2. The supplied D: workspace had no free space; the checkout, builds, and
raw records are on C:.

The integrated build passes all 16 portable Release and all 16 native Release
CTest targets. All six C++ targets pass in Debug with ASan and UBSan, LTO
disabled. Perft regressions and a legal-move cross-check on nine curated plus
40 random positions (ten plies, seed 17) pass. Python tooling compiles.

The first implementation passed all 13 portable/native Release CTest targets,
all three C++ ASan/UBSan targets, perft regressions, differential perft on 1,000
positions at depth two, and legal-move checks on nine curated plus 100 random
positions. It also passed 1,600 repeatable skill-20 choices (31 independently
confirmed alternatives) and 12 complete legal games over 1,211 searched plies.
Those records describe the earlier implementation before the club default,
repertoire integration, learned prior, and additional planning changes.

The integrated club default passes 1,600 choices on the first 50 bundled UHO
positions, depth four, 1 MB hash, book enabled, seeds 1–32. Every choice repeats
after resetting the seed and search state. All 226 alternatives pass a fresh
restricted-search score check; all 85 intentional losses over 25 cp pass the
independent shallow plausibility check. The maximum measured loss is 72 cp
against the main search's completed score, within the 76 cp allowance.

The same profile completes 12 legal games from six paired match openings,
with colors reversed, 1,500 nodes per move, book enabled, and seeds 20261023
and 20261024. There are 1,333 searched plies: ten checkmates, one threefold
repetition, and one fifty-move draw. Final PVs agree with played moves and
reported node counts remain within the limit.

Native prior parity passes on 100 committed golden positions and 2,352 legal
moves, including both colors, castling, en passant, all promotions, pinned
attackers, and material-phase thresholds. A separate seeded stress check passes
on 1,093 positions and 30,191 moves, with logit differences below 5e-8 at the
probe's printed precision. The committed golden check runs without Python.

The reproducible selector check uses the actual UCI `bestmove`, final PV, fresh
restricted confirmation, and the main search's completed score and shallow
iteration:

```sh
python tools/check_human_play.py build/release/proton_chess --json results/club-validation.json
```

The allowance is relative to this engine's completed search; it does not bound
loss against deeper analysis or another engine. Gameplay checks establish
legality and integration, not playing strength. No new Elo claim is made.
