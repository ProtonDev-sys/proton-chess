# Human club move-prior research

This optional offline pipeline fits a 53-feature conditional logistic model to
recorded Lichess moves. Python, NumPy, SciPy, python-chess and zstandard are
training dependencies only. The engine needs none of these packages at runtime.
The portable weights, feature ordering and provenance are in
[`../../models/human-club-v1.json`](../../models/human-club-v1.json).
No game archive, positions, binary scorer or feature matrix belongs in Git.

Run from the repository root. Paths are arguments; no machine-specific paths
are required. Example on Linux/WSL:

```sh
python3 -m venv /tmp/proton-human-venv
/tmp/proton-human-venv/bin/python -m pip install -r tools/human_policy/requirements.txt
/tmp/proton-human-venv/bin/python tools/human_policy/download.py --out-dir results/human-research
/tmp/proton-human-venv/bin/python tools/human_policy/extract.py --out-dir results/human-research
/tmp/proton-human-venv/bin/python tools/human_policy/features.py --out-dir results/human-research
/tmp/proton-human-venv/bin/python tools/human_policy/train.py --out-dir results/human-research
/tmp/proton-human-venv/bin/python tools/human_policy/audit.py --out-dir results/human-research --reference-model models/human-club-v1.json
```

Each command supports `--help`. Training omits the optional handmade-policy
comparison unless `--handcrafted-binary PATH` is supplied. That switch does not
affect fitted weights. `--model-out PATH` optionally copies the fitted JSON.

The published CLI pipeline was rerun in a fresh output directory: every fitted
weight, training/validation/test metric, and baseline calibration multiplier
matched exactly. A local HTTP200 server that ignored Range also confirmed the
downloader's hard byte cap. Verification artifacts stay ignored under
`results/human-research/reproduction/`.

## Data and filtering

The fixed source is the September 2026 standard rated monthly archive at
`https://database.lichess.org/standard/lichess_db_standard_rated_2026-09.pgn.zst`.
Download requests `Range: bytes=0-33554431`, reads at most 33,554,432 compressed
bytes and closes immediately even if the server ignores Range. It cannot
download the complete archive. The expected prefix SHA-256 is
`d629023c6ceb487afcd676cc840e744a1f97b8f8782e9f0af3f846998fa205f6`.
The original response was HTTP 206, with
`Content-Range: bytes 0-33554431/29224520887`.
`--url`, `--max-mib` (1..32), and `--expected-sha256` permit a different bounded
experiment; supplying a different prefix requires its expected hash or an
explicit empty hash argument. Such experiments do not reproduce v1.

Extraction streams the incomplete zstd frame and accepts a PGN only when the
next Event header terminates it. It always discards the final partial game.
The prefix contains 102,432 complete games and decodes 239,599,616 bytes.
Filtering accepts only exact Event values `Rated Rapid game` and
`Rated Classical game`, orthodox starting positions, both integer ratings in
1800..2200 inclusive, completed results, valid time controls, and no BOT title.
This intentionally omits rapid/classical tournament and swiss Event formats.
It selects 2,290 games: 2,241 rapid and 49 classical. Ten BOT-title games were
excluded after 2,300 games passed the rating filter.

Decisions on plies 1..12 are excluded. The mover's previous recorded clock
(initially the base time) and current postmove clock minus increment must both
be known and at least 30 seconds. The latter prevents increment from hiding
time pressure. In selected games there are 166,510 plies: 27,429 opening plies,
3,819 further clock exclusions, and 135,262 eligible decisions. The first
extraction keeps at most 40 decisions/game by smallest
`sha256(seed:game_id:ply)`, then feature generation keeps the smallest 12 using
the same rule. Choosing from the 40 produces exactly the same 12 as choosing
directly from all eligible decisions. Move labels never enter the sample hash.
Fourteen selected games have no eligible decisions.

The deterministic seed is `proton-human-prior-v1-20261023`. Game Site is the
game ID; `sha256(seed:game_id)` first 32 bits modulo100 assigns <70 train, <85
validation, otherwise test. It produces 27,001 decisions / 864,339 legal
candidates from 2,276 games: 18,731/4,137/4,133 decisions in 1,578/349/349 games.
`audit.py` rejects duplicate selected game IDs, duplicate game/ply decisions,
and crossing game IDs; it reports full-FEN and first-four-field FEN crossing.
Eight test states also occur in training/validation; the separately reported
novel-state test keeps 4,125 decisions and has effectively identical results.

## Features, fit and inference

[`features.py`](features.py) is the executable feature specification:
`NAMES` fixes the 53 feature order and `candidate_features` defines each value.
Features cover moving piece, captures/promotion, geometric attackers and
defenders, escaping attack, checks, centralization/activity, development,
rook files, passed pawns, king phase and material-sensitive exchanges.
Piece values are pawn 1, knight 3.2, bishop 3.3, rook 5, queen 9, king 0. Attackers
are geometric, so pinned pieces can count; this is not SEE or engine search.

Fit uses the recorded move as a label and every legal move as an alternative:
`p(move|position) = softmax(sum_j weight[j] * feature[j])`.
Feature scales are training-candidate standard deviations, floored at 0.05.
L-BFGS-B fits average conditional NLL plus 0.5*L2*sum(standardized_weight^2).
The fixed L2 grid 0.01/0.001/0.0001 is selected only by validation NLL; v1
selects 0.0001. The published weights already divide out feature scales:
**native inference uses the raw features directly, without standardization**.
There are 53 weights, 212 bytes as float32. Metadata uses full-precision JSON.
No intercept is needed because a position-constant term cancels in softmax.
Python 3.12.3 and the pinned training dependencies reproduced the experiment.
BLAS threading is fixed to one; optimization has no random initialization.

Held-out test: learned top-1=0.3197435277/NLL=2.4069576235 nats;
handmade top-1=0.2109793801/NLL=3.0920172266;
captures/checks top-1=0.1892508445/NLL=3.0560970264;
uniform top-1=0.0630219524/NLL=3.2879444490.
Top1 means expected accuracy with uniform random tie-breaking among maximal
scores. All models see identical legal alternatives; 41 forced test decisions
are included. Without forced choices, learned top-1=0.3129276637 versus
handmade 0.2030737482. The learned-minus-handmade top-1 gain is 0.1087641476,
with a game-cluster bootstrap 95% CI[0.0958110851,0.1219353069].
The test set is never used for fitting, calibration or grid selection.
Later subgroup audits are descriptive and do not change weights.

## Optional genuine handmade baseline protocol

Pass `--handcrafted-binary PATH` to `train.py`. The executable must read one
six-field FEN per line, and return exactly one JSON line per input:

```json
{"fen":"CANONICAL_FEN","moves":[{"uci":"e2e4","score":1.25}]}
```

The binary may accept the entire stream before returning. It must return raw
`Evaluator::policy_scores`, with every legal UCI move exactly once. Training
checks the FEN and legal-move set at every position; candidate order may differ.
The original scorer used an immutable source snapshot identified in the model
manifest. A changed policy is a different comparison baseline. Only the
handmade score multiplier and captures/checks multiplier are optimized on
validation; neither changes learned weights. Actual engine root search,
book/selector terms and bounded confirmation are outside this comparison.
The simple logit is captured pawn-value minus0.1*mover-value for captures,
plus 2 for check and promotion gain; it also receives validation-only scaling.

## License and limits

The first-party database page `https://database.lichess.org/` states:
"Database exports are released under the Creative Commons CC0 license" and
"You can download, modify and redistribute them, without asking for permission."
The archived license HTML SHA-256 and HTTP provenance appear in the model
manifest. These are standard monthly games; broadcasts have a separate license.

All selected games come from September 1, 2026. This contiguous prefix is not a
random monthly sample. Most test games share a player with training; the 42
test games with both players unseen (504 decisions) still favor learned
top-1=0.2996 versus handmade 0.1872, but are small. Classical test coverage is
only 132 decisions. Ratings are Lichess pool ratings. BOT-title exclusion cannot
certify absence of undisclosed computer assistance. The recorded clock filter
does not reconstruct lag or thinking time.

This establishes out-of-game prediction uplift for a small learned move prior.
Safety features may explain much of the gain; it does not establish style
independently of tactical plausibility. It does not establish engine strength,
subjective human resemblance, or human indistinguishability. Applying weights
inside a search-safe subset changes the choice distribution; the standalone
prediction result does not measure the final engine's move accuracy. Native
parity and selector/gameplay checks are recorded in the
[integration report](../../docs/human-play-2026-10-23.md). No new test-set tuning
is attributed to this fixed v1 result.
