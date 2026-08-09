#!/usr/bin/env python3
"""Run a reproducible, symmetric match between two Proton builds."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import shutil
import sys
import tempfile
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

import chess
import chess.engine

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import estimate_elo  # noqa: E402


CONFIGURATION_ORDER = (
    "Hash",
    "Threads",
    "UseBook",
    "BookRandomness",
    "Skill Level",
    "HumanStyle",
    "HumanSkill",
    "HumanMaxLossCp",
    "HumanSeed",
    "MoveOverhead",
    "Contempt",
    "UCI_LimitStrength",
)
ENGINE_SEED_DERIVATION = (
    "SHA-256 of NUL-separated proton-engine-ab-seed-v1, match seed, pair and "
    "candidate color; first 8 bytes as big-endian unsigned integer; zero maps to one; "
    "the same seed is sent to both engines"
)


@dataclass(frozen=True)
class LabeledEngineArtifact:
    label: str
    name: str
    author: str
    path: str
    sha256: str


@dataclass(frozen=True)
class EngineProfile:
    artifact: LabeledEngineArtifact
    supported_options: tuple[str, ...]


@dataclass
class AbGameRecord:
    pair: int
    candidate_color: str
    opening_index: int
    opening_fen: str
    opening_moves: list[str]
    point: float
    termination: str
    plies: int
    moves: list[str]
    white_clock_seconds: float | None
    black_clock_seconds: float | None
    engine_seed: str | None


@dataclass
class AbResult:
    games: int
    wins: int
    draws: int
    losses: int
    score: float
    estimated_elo_delta: float
    pair_count: int
    pair_scores: list[float]
    complete_pair_score: float | None
    pentanomial_counts: dict[str, int]
    score_ci95_low: float
    score_ci95_high: float
    exact_p_gain: float
    exact_p_regression: float
    exact_alpha: float
    verdict: str
    significant_above_50: bool
    records: list[AbGameRecord]


@dataclass
class AbReport:
    schema_version: int
    match_type: str
    status: str
    created_utc: str
    completed_utc: str | None
    seed: int
    requested_games: int
    scheduled_games: int
    exact_alpha: float
    max_plies: int
    hash_mb: int
    time_control: dict[str, float | str | None]
    candidate: LabeledEngineArtifact
    baseline: LabeledEngineArtifact
    candidate_options: dict[str, Any]
    baseline_options: dict[str, Any]
    engine_seed_derivation: str | None
    openings: estimate_elo.InputArtifact
    tool: estimate_elo.InputArtifact
    match_core: estimate_elo.InputArtifact
    git_revision: str | None
    git_dirty: bool | None
    python_version: str
    python_chess_version: str
    host: dict[str, Any]
    statistical_unit: str
    opening_selection: str
    lifecycle: str
    confidence_level: float
    confidence_assumptions: list[str]
    result: AbResult | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a symmetric paired match between candidate and baseline Proton builds."
    )
    parser.add_argument("candidate", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument("--games", type=int, default=200,
                        help="total games, rounded up to a complete color-swapped pair")
    timing = parser.add_mutually_exclusive_group()
    timing.add_argument("--move-time", type=float,
                        help="fixed seconds per move (default: 0.05)")
    timing.add_argument("--base-seconds", type=float,
                        help="Fischer starting clock in seconds")
    parser.add_argument("--increment", type=float, default=0.0,
                        help="Fischer increment in seconds")
    parser.add_argument("--watchdog-grace", type=float, default=2.0,
                        help="extra wall-clock seconds before killing a stuck engine")
    parser.add_argument("--max-plies", type=int, default=240)
    parser.add_argument("--hash", type=int, default=16)
    parser.add_argument("--seed", type=int, default=20260809)
    parser.add_argument("--alpha", type=float, default=0.05,
                        help="one-sided exact-test threshold for the final verdict")
    parser.add_argument("--openings", type=Path,
                        default=Path("openings/uho_lichess_4852_v1_200.epd"))
    parser.add_argument("--json", type=Path, dest="json_path", required=True,
                        help="atomic checkpoint and final JSON report path")
    return parser.parse_args()


def derive_engine_seed(match_seed: int, pair: int, candidate_color: str) -> int:
    if candidate_color not in {"white", "black"}:
        raise ValueError("candidate color must be white or black")
    fields = (
        "proton-engine-ab-seed-v1",
        str(match_seed),
        str(pair),
        candidate_color,
    )
    digest = hashlib.sha256("\0".join(fields).encode("utf-8")).digest()
    return int.from_bytes(digest[:8], byteorder="big") or 1


def stage_file(source: Path, destination: Path) -> str:
    source_hash_before = estimate_elo.sha256_file(source)
    shutil.copy2(source, destination)
    staged_hash = estimate_elo.sha256_file(destination)
    source_hash_after = estimate_elo.sha256_file(source)
    if source_hash_before != staged_hash or source_hash_after != staged_hash:
        destination.unlink(missing_ok=True)
        raise ValueError(f"input changed while it was staged: {source}")
    return staged_hash


def inspect_engine(
    staged_path: Path,
    label: str,
    source_path: Path,
    expected_sha256: str,
) -> EngineProfile:
    if estimate_elo.sha256_file(staged_path) != expected_sha256:
        raise ValueError(f"staged engine hash changed before inspection: {label}")
    engine = chess.engine.SimpleEngine.popen_uci(str(staged_path))
    try:
        identity = dict(engine.id)
        supported = tuple(name for name in CONFIGURATION_ORDER if name in engine.options)
    finally:
        estimate_elo.safe_quit(engine)
    return EngineProfile(
        artifact=LabeledEngineArtifact(
            label=label,
            name=str(identity.get("name", source_path.stem)),
            author=str(identity.get("author", "")),
            path=str(source_path),
            sha256=expected_sha256,
        ),
        supported_options=supported,
    )


def validate_profiles(candidate: EngineProfile, baseline: EngineProfile) -> None:
    if candidate.artifact.sha256 == baseline.artifact.sha256:
        raise ValueError("candidate and baseline binaries must have different SHA-256 hashes")
    if candidate.supported_options != baseline.supported_options:
        raise ValueError(
            "candidate and baseline do not support the same controlled UCI options: "
            f"{candidate.supported_options!r} != {baseline.supported_options!r}"
        )
    missing = [name for name in CONFIGURATION_ORDER
               if name not in candidate.supported_options]
    if missing:
        raise ValueError(
            "both engines must advertise every controlled Proton option; missing "
            + ", ".join(missing)
        )


def engine_configuration(
    supported_options: tuple[str, ...], hash_mb: int, engine_seed: int
) -> dict[str, Any]:
    values: dict[str, Any] = {
        "Hash": hash_mb,
        "Threads": 1,
        "UseBook": False,
        "BookRandomness": 0,
        "Skill Level": 20,
        "HumanStyle": False,
        "HumanSkill": 20,
        "HumanMaxLossCp": 12,
        "HumanSeed": str(engine_seed),
        "MoveOverhead": 25,
        "Contempt": 0,
        "UCI_LimitStrength": False,
    }
    return {name: values[name] for name in CONFIGURATION_ORDER if name in supported_options}


def configure_pair(
    candidate: chess.engine.SimpleEngine,
    baseline: chess.engine.SimpleEngine,
    supported_options: tuple[str, ...],
    hash_mb: int,
    engine_seed: int,
) -> dict[str, Any]:
    if tuple(name for name in CONFIGURATION_ORDER if name in candidate.options) != supported_options:
        raise ValueError("candidate UCI options changed after initial inspection")
    if tuple(name for name in CONFIGURATION_ORDER if name in baseline.options) != supported_options:
        raise ValueError("baseline UCI options changed after initial inspection")
    configuration = engine_configuration(supported_options, hash_mb, engine_seed)
    candidate.configure(configuration)
    baseline.configure(configuration)
    return configuration


def complete_pair_scores(records: list[AbGameRecord]) -> list[float]:
    grouped: dict[int, list[AbGameRecord]] = {}
    for record in records:
        grouped.setdefault(record.pair, []).append(record)

    scores: list[float] = []
    for pair in sorted(grouped):
        pair_records = grouped[pair]
        if len(pair_records) == 1:
            continue
        if len(pair_records) != 2:
            raise ValueError(f"pair {pair} has {len(pair_records)} games")
        if {record.candidate_color for record in pair_records} != {"white", "black"}:
            raise ValueError(f"pair {pair} does not contain both candidate colors")
        opening_keys = {
            (record.opening_index, record.opening_fen, tuple(record.opening_moves))
            for record in pair_records
        }
        if len(opening_keys) != 1:
            raise ValueError(f"pair {pair} uses different openings")
        scores.append(sum(record.point for record in pair_records) / 2.0)
    return scores


def exact_sign_flip_probabilities(pair_scores: list[float]) -> tuple[float, float]:
    centered: list[int] = []
    for score in pair_scores:
        units = round(4.0 * (score - 0.5))
        if units < -2 or units > 2 or abs(4.0 * (score - 0.5) - units) > 1e-9:
            raise ValueError(f"invalid paired score: {score}")
        if units != 0:
            centered.append(units)

    observed = sum(centered)
    distribution: dict[int, float] = {0: 1.0}
    for magnitude in map(abs, centered):
        next_distribution: dict[int, float] = {}
        for total, probability in distribution.items():
            next_distribution[total + magnitude] = (
                next_distribution.get(total + magnitude, 0.0) + probability / 2.0
            )
            next_distribution[total - magnitude] = (
                next_distribution.get(total - magnitude, 0.0) + probability / 2.0
            )
        distribution = next_distribution
    p_gain = sum(probability for total, probability in distribution.items()
                 if total >= observed)
    p_regression = sum(probability for total, probability in distribution.items()
                       if total <= observed)
    return p_gain, p_regression


def summarize(
    records: list[AbGameRecord], alpha: float, *, final: bool = False
) -> AbResult:
    if not records:
        raise ValueError("cannot summarize an empty A/B match")
    points = [record.point for record in records]
    if any(point not in {0.0, 0.5, 1.0} for point in points):
        raise ValueError("game points must be 0, 0.5, or 1")
    score = sum(points) / len(points)
    pairs = complete_pair_scores(records)
    ci_low, ci_high = estimate_elo.hoeffding_interval(pairs)
    p_gain, p_regression = exact_sign_flip_probabilities(pairs)
    verdict = "running" if not final else "inconclusive"
    if final and score > 0.5 and p_gain <= alpha:
        verdict = "gain"
    elif final and score < 0.5 and p_regression <= alpha:
        verdict = "regression"
    return AbResult(
        games=len(points),
        wins=points.count(1.0),
        draws=points.count(0.5),
        losses=points.count(0.0),
        score=score,
        estimated_elo_delta=estimate_elo.logistic_elo(0, score),
        pair_count=len(pairs),
        pair_scores=pairs,
        complete_pair_score=(sum(pairs) / len(pairs)) if pairs else None,
        pentanomial_counts=estimate_elo.pentanomial_counts(pairs),
        score_ci95_low=ci_low,
        score_ci95_high=ci_high,
        exact_p_gain=p_gain,
        exact_p_regression=p_regression,
        exact_alpha=alpha,
        verdict=verdict,
        significant_above_50=final and ci_low > 0.5,
        records=list(records),
    )


def run_match(
    candidate_path: Path,
    baseline_path: Path,
    supported_options: tuple[str, ...],
    openings: list[estimate_elo.OpeningPosition],
    args: argparse.Namespace,
    time_control: estimate_elo.TimeControl,
    checkpoint: Callable[[AbResult], None] | None = None,
) -> AbResult:
    games = args.games + args.games % 2
    pair_count = games // 2
    selected = estimate_elo.select_openings(openings, pair_count, args.seed)
    records: list[AbGameRecord] = []

    for pair_index, (opening_index, opening) in enumerate(selected, start=1):
        for candidate_is_white in (True, False):
            candidate_color = "white" if candidate_is_white else "black"
            engine_seed = (
                derive_engine_seed(args.seed, pair_index, candidate_color)
                if "HumanSeed" in supported_options else None
            )
            candidate: chess.engine.SimpleEngine | None = None
            baseline: chess.engine.SimpleEngine | None = None
            try:
                candidate = chess.engine.SimpleEngine.popen_uci(str(candidate_path))
                baseline = chess.engine.SimpleEngine.popen_uci(str(baseline_path))
                game_token = (
                    "candidate-vs-baseline",
                    args.seed,
                    pair_index,
                    candidate_color,
                )
                configure_pair(
                    candidate,
                    baseline,
                    supported_options,
                    args.hash,
                    1 if engine_seed is None else engine_seed,
                )
                estimate_elo.prepare_engine_for_game(candidate, game_token)
                estimate_elo.prepare_engine_for_game(baseline, game_token)
                outcome = estimate_elo.play_game(
                    candidate,
                    baseline,
                    opening,
                    candidate_is_white,
                    time_control,
                    args.max_plies,
                    game_token,
                )
            finally:
                if candidate is not None:
                    estimate_elo.safe_quit(candidate)
                if baseline is not None:
                    estimate_elo.safe_quit(baseline)

            records.append(AbGameRecord(
                pair=pair_index,
                candidate_color=candidate_color,
                opening_index=opening_index,
                opening_fen=opening.fen,
                opening_moves=list(opening.moves),
                point=outcome.point,
                termination=outcome.termination,
                plies=outcome.plies,
                moves=outcome.moves,
                white_clock_seconds=outcome.white_clock_seconds,
                black_clock_seconds=outcome.black_clock_seconds,
                engine_seed=None if engine_seed is None else str(engine_seed),
            ))
            partial = summarize(records, args.alpha)
            if checkpoint is not None:
                checkpoint(partial)
            print(
                f"pair={pair_index}/{pair_count} candidate={candidate_color} "
                f"result={outcome.point:g} termination={outcome.termination} "
                f"score={sum(record.point for record in records):g}/{len(records)}",
                flush=True,
            )

    return summarize(records, args.alpha, final=True)


def reported_options(
    supported_options: tuple[str, ...], hash_mb: int
) -> dict[str, Any]:
    configuration = engine_configuration(supported_options, hash_mb, 1)
    if "HumanSeed" in configuration:
        configuration["HumanSeed"] = "per-game; see result.records[].engine_seed"
    return configuration


def main() -> int:
    args = parse_args()
    candidate_path = args.candidate.resolve()
    baseline_path = args.baseline.resolve()
    openings_path = args.openings.resolve()
    candidate_label = args.candidate_label.strip()
    baseline_label = args.baseline_label.strip()
    if not candidate_path.is_file() or not baseline_path.is_file():
        raise FileNotFoundError("both engine paths must be files")
    if candidate_path == baseline_path:
        raise ValueError("candidate and baseline paths must be different")
    if not openings_path.is_file():
        raise FileNotFoundError(openings_path)
    if not candidate_label or not baseline_label or candidate_label == baseline_label:
        raise ValueError("candidate and baseline labels must be non-empty and different")
    if args.games < 2 or args.max_plies < 40 or args.hash < 1:
        raise ValueError("games, max-plies, or hash is outside its valid range")
    if not 0.0 < args.alpha < 1.0:
        raise ValueError("alpha must be between zero and one")

    time_control = estimate_elo.build_time_control(args)
    tool_path = Path(__file__).resolve()
    core_path = Path(estimate_elo.__file__).resolve()
    json_path = args.json_path.resolve()
    with tempfile.TemporaryDirectory(prefix="proton-ab-") as temporary:
        staging_root = Path(temporary)
        staged_candidate = staging_root / f"candidate{candidate_path.suffix}"
        staged_baseline = staging_root / f"baseline{baseline_path.suffix}"
        staged_openings = staging_root / f"openings{openings_path.suffix}"
        candidate_hash = stage_file(candidate_path, staged_candidate)
        baseline_hash = stage_file(baseline_path, staged_baseline)
        openings_hash = stage_file(openings_path, staged_openings)

        candidate_profile = inspect_engine(
            staged_candidate, candidate_label, candidate_path, candidate_hash
        )
        baseline_profile = inspect_engine(
            staged_baseline, baseline_label, baseline_path, baseline_hash
        )
        validate_profiles(candidate_profile, baseline_profile)
        openings = estimate_elo.load_openings(staged_openings)
        if estimate_elo.sha256_file(staged_openings) != openings_hash:
            raise ValueError("staged opening file changed while it was loaded")

        report = AbReport(
            schema_version=1,
            match_type="candidate_vs_baseline",
            status="running",
            created_utc=datetime.now(timezone.utc).isoformat(timespec="seconds"),
            completed_utc=None,
            seed=args.seed,
            requested_games=args.games,
            scheduled_games=args.games + args.games % 2,
            exact_alpha=args.alpha,
            max_plies=args.max_plies,
            hash_mb=args.hash,
            time_control=time_control.payload(),
            candidate=candidate_profile.artifact,
            baseline=baseline_profile.artifact,
            candidate_options=reported_options(
                candidate_profile.supported_options, args.hash
            ),
            baseline_options=reported_options(
                baseline_profile.supported_options, args.hash
            ),
            engine_seed_derivation=(
                ENGINE_SEED_DERIVATION
                if "HumanSeed" in candidate_profile.supported_options else None
            ),
            openings=estimate_elo.InputArtifact(
                path=str(openings_path), sha256=openings_hash
            ),
            tool=estimate_elo.InputArtifact(
                path=str(tool_path), sha256=estimate_elo.sha256_file(tool_path)
            ),
            match_core=estimate_elo.InputArtifact(
                path=str(core_path), sha256=estimate_elo.sha256_file(core_path)
            ),
            git_revision=estimate_elo.git_revision(ROOT),
            git_dirty=estimate_elo.git_dirty(ROOT),
            python_version=platform.python_version(),
            python_chess_version=getattr(chess, "__version__", "unknown"),
            host={
                "platform": platform.platform(),
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "processor": platform.processor(),
                "logical_cpu_count": os.cpu_count(),
            },
            statistical_unit="one color-swapped opening pair",
            opening_selection=(
                "seeded sample without replacement until the opening suite is exhausted; "
                "additional pairs use seeded draws with replacement"
            ),
            lifecycle=(
                "verified private input copies; fresh candidate and baseline processes "
                "per game; identical options; ucinewgame and isready complete before "
                "the clock starts"
            ),
            confidence_level=0.95,
            confidence_assumptions=[
                "The configured game count and exact-test alpha are fixed before play; "
                "running checkpoints never issue a final verdict.",
                "Candidate and baseline are configured with the same explicit "
                "full-strength options and the same per-game seed.",
                "The two games within each color-swapped pair are not treated as "
                "independent.",
                "The exact sign-flip test assumes engine labels are exchangeable under "
                "the null.",
                "Residual timing and shared-host effects are assumed not to induce "
                "cross-pair dependence.",
            ],
            result=None,
        )
        estimate_elo.write_json_atomic(json_path, asdict(report))

        def checkpoint(partial: AbResult) -> None:
            report.result = partial
            estimate_elo.write_json_atomic(json_path, asdict(report))

        report.result = run_match(
            staged_candidate,
            staged_baseline,
            candidate_profile.supported_options,
            openings,
            args,
            time_control,
            checkpoint=checkpoint,
        )
        for label, staged_path, expected_hash in (
            ("candidate", staged_candidate, candidate_hash),
            ("baseline", staged_baseline, baseline_hash),
            ("openings", staged_openings, openings_hash),
        ):
            if estimate_elo.sha256_file(staged_path) != expected_hash:
                raise ValueError(f"staged {label} changed during the match")
        report.status = "complete"
        report.completed_utc = datetime.now(timezone.utc).isoformat(timespec="seconds")
        rendered = json.dumps(asdict(report), indent=2)
        estimate_elo.write_json_atomic(json_path, asdict(report))
        print(rendered)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (chess.engine.EngineError, chess.engine.EngineTerminatedError) as error:
        print(f"engine failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
