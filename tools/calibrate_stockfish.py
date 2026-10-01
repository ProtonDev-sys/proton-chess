#!/usr/bin/env python3
"""Run one reproducible fixed-node Proton versus Stockfish calibration shard."""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import platform
import random
import statistics
import sys
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import chess
import chess.engine

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import estimate_elo  # noqa: E402

MAX_PAIRS_PER_SHARD = 50


@dataclass(frozen=True)
class FixedNodeTimeControl:
    nodes_per_move: int
    watchdog_grace_seconds: float
    base_seconds: None = None
    increment_seconds: float = 0.0

    def limit(self, clocks: dict[chess.Color, float] | None) -> chess.engine.Limit:
        del clocks
        return chess.engine.Limit(nodes=self.nodes_per_move)

    def watchdog(
        self, side: chess.Color, clocks: dict[chess.Color, float] | None
    ) -> float:
        del side, clocks
        return max(0.001, self.watchdog_grace_seconds)

    def payload(self) -> dict[str, float | int | str | None]:
        return {
            "mode": "fixed_nodes",
            "nodes_per_move": self.nodes_per_move,
            "move_time_seconds": None,
            "base_seconds": None,
            "increment_seconds": 0.0,
            "watchdog_grace_seconds": self.watchdog_grace_seconds,
        }


@dataclass(frozen=True)
class OpponentLevel:
    label: str
    requested_elo: int | None
    effective_elo: int | None
    limited: bool


@dataclass
class CalibrationResult:
    opponent_label: str
    opponent_elo_requested: int | None
    opponent_elo_effective: int | None
    full_strength: bool
    games: int
    wins: int
    draws: int
    losses: int
    score: float
    estimated_elo: float | None
    pair_count: int
    pair_scores: list[float]
    complete_pair_score: float | None
    pentanomial_counts: dict[str, int]
    confidence_method: str
    score_ci95_low: float
    score_ci95_high: float
    significant_above_50: bool
    observed_score_at_least_70: bool
    records: list[estimate_elo.GameRecord]


@dataclass
class CalibrationShardReport:
    schema_version: int
    match_type: str
    status: str
    created_utc: str
    completed_utc: str
    seed: int
    pair_offset: int
    pair_count: int
    max_plies: int
    hash_mb: int
    time_control: dict[str, float | int | str | None]
    proton: estimate_elo.EngineArtifact
    stockfish: estimate_elo.EngineArtifact
    stockfish_uci_elo_range: dict[str, int | None]
    opponent: dict[str, Any]
    proton_options: dict[str, Any]
    stockfish_options: dict[str, Any]
    openings: estimate_elo.InputArtifact
    opening_order: str
    selected_opening_indices: list[int]
    tool: estimate_elo.InputArtifact
    match_core: estimate_elo.InputArtifact
    proton_source_commit: str | None
    stockfish_source_ref: str
    python_version: str
    python_chess_version: str
    host: dict[str, Any]
    statistical_unit: str
    engine_lifecycle: str
    result: CalibrationResult


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run one color-swapped fixed-node calibration shard against an "
            "official Stockfish limited-strength level or full strength."
        )
    )
    parser.add_argument("proton", type=Path)
    parser.add_argument("stockfish", type=Path)
    level = parser.add_mutually_exclusive_group(required=True)
    level.add_argument("--opponent-elo", type=int)
    level.add_argument("--full-strength", action="store_true")
    parser.add_argument("--pairs", type=int, default=50)
    parser.add_argument("--offset", type=int, default=0)
    parser.add_argument("--nodes", type=int, default=1000)
    parser.add_argument("--watchdog-grace", type=float, default=2.0)
    parser.add_argument("--max-plies", type=int, default=200)
    parser.add_argument("--hash", type=int, default=16)
    parser.add_argument("--seed", type=int, default=20260823)
    parser.add_argument("--proton-commit")
    parser.add_argument("--stockfish-ref", default="sf_18")
    parser.add_argument(
        "--openings",
        type=Path,
        default=Path("openings/uho_lichess_4852_v1_200.epd"),
    )
    parser.add_argument("--json", type=Path, dest="json_path", required=True)
    return parser.parse_args()


def build_time_control(args: argparse.Namespace) -> FixedNodeTimeControl:
    if args.nodes <= 0:
        raise ValueError("nodes must be positive")
    if args.watchdog_grace <= 0.0:
        raise ValueError("watchdog-grace must be positive")
    return FixedNodeTimeControl(args.nodes, args.watchdog_grace)


def resolve_level(
    requested_elo: int | None,
    full_strength: bool,
    elo_range: estimate_elo.UciEloRange,
) -> OpponentLevel:
    if full_strength:
        return OpponentLevel("full-strength", None, None, False)
    if requested_elo is None:
        raise ValueError("an opponent level is required")
    resolved = estimate_elo.resolve_opponent_levels([requested_elo], elo_range)[0]
    return OpponentLevel(
        str(resolved.effective), resolved.requested, resolved.effective, True
    )


def select_opening_shard(
    openings: list[estimate_elo.OpeningPosition],
    pair_count: int,
    offset: int,
    seed: int,
) -> list[tuple[int, estimate_elo.OpeningPosition]]:
    if pair_count < 1 or pair_count > MAX_PAIRS_PER_SHARD:
        raise ValueError(f"pairs must be between 1 and {MAX_PAIRS_PER_SHARD}")
    if offset < 0 or offset + pair_count > len(openings):
        raise ValueError("offset + pairs exceeds the unique opening suite")
    indexed = list(enumerate(openings))
    random.Random(seed).shuffle(indexed)
    return [
        (index, estimate_elo.OpeningPosition(opening.fen, list(opening.moves)))
        for index, opening in indexed[offset:offset + pair_count]
    ]


def configure_engines(
    proton: chess.engine.SimpleEngine,
    stockfish: chess.engine.SimpleEngine,
    level: OpponentLevel,
    hash_mb: int,
) -> tuple[dict[str, Any], dict[str, Any]]:
    proton_configuration: dict[str, Any] = {
        "Hash": hash_mb,
        "UseBook": False,
        "HumanStyle": False,
        "UCI_LimitStrength": False,
    }
    stockfish_configuration: dict[str, Any] = {
        "Hash": hash_mb,
        "Threads": 1,
        "UCI_LimitStrength": level.limited,
    }
    if level.limited:
        assert level.effective_elo is not None
        stockfish_configuration["UCI_Elo"] = level.effective_elo
    proton.configure(proton_configuration)
    stockfish.configure(stockfish_configuration)
    return proton_configuration, stockfish_configuration


def close_engine(engine: chess.engine.SimpleEngine | None) -> None:
    if engine is None:
        return
    if not hasattr(engine, "quit") or not hasattr(engine, "timeout"):
        engine.close()
        return
    previous_timeout = engine.timeout
    engine.timeout = 2.0
    try:
        engine.quit()
    except (
        chess.engine.EngineError,
        TimeoutError,
        concurrent.futures.CancelledError,
        OSError,
    ):
        engine.close()
    finally:
        engine.timeout = previous_timeout
        try:
            engine.returncode.result(timeout=2.0)
        except (
            concurrent.futures.TimeoutError,
            concurrent.futures.CancelledError,
            OSError,
        ):
            engine.close()


def summarize_records(
    level: OpponentLevel,
    records: list[estimate_elo.GameRecord],
) -> CalibrationResult:
    if not records:
        raise ValueError("cannot summarize an empty calibration shard")
    points = [record.point for record in records]
    pairs = estimate_elo.complete_pair_scores(records)
    ci_low, ci_high = estimate_elo.hoeffding_interval(pairs)
    score = statistics.fmean(points)
    estimated_elo = (
        None
        if level.effective_elo is None
        else estimate_elo.logistic_elo(level.effective_elo, score)
    )
    return CalibrationResult(
        opponent_label=level.label,
        opponent_elo_requested=level.requested_elo,
        opponent_elo_effective=level.effective_elo,
        full_strength=not level.limited,
        games=len(points),
        wins=points.count(1.0),
        draws=points.count(0.5),
        losses=points.count(0.0),
        score=score,
        estimated_elo=estimated_elo,
        pair_count=len(pairs),
        pair_scores=pairs,
        complete_pair_score=statistics.fmean(pairs) if pairs else None,
        pentanomial_counts=estimate_elo.pentanomial_counts(pairs),
        confidence_method="hoeffding_two_sided_95_over_opening_pairs",
        score_ci95_low=ci_low,
        score_ci95_high=ci_high,
        significant_above_50=ci_low > 0.5,
        observed_score_at_least_70=score >= 0.7,
        records=list(records),
    )


def run_shard(
    proton_path: Path,
    stockfish_path: Path,
    level: OpponentLevel,
    selected_openings: list[tuple[int, estimate_elo.OpeningPosition]],
    args: argparse.Namespace,
    time_control: FixedNodeTimeControl,
) -> CalibrationResult:
    proton = chess.engine.SimpleEngine.popen_uci(str(proton_path))
    stockfish = chess.engine.SimpleEngine.popen_uci(str(stockfish_path))
    records: list[estimate_elo.GameRecord] = []
    try:
        configure_engines(proton, stockfish, level, args.hash)
        for local_pair, (opening_index, opening) in enumerate(
            selected_openings, start=1
        ):
            global_pair = args.offset + local_pair
            for proton_is_white in (True, False):
                proton_color = "white" if proton_is_white else "black"
                game_token = (
                    "stockfish-fixed-node-calibration-shard-v1",
                    args.seed,
                    level.label,
                    global_pair,
                    proton_color,
                )
                estimate_elo.prepare_engine_for_game(proton, game_token)
                estimate_elo.prepare_engine_for_game(stockfish, game_token)
                outcome = estimate_elo.play_game(
                    proton,
                    stockfish,
                    opening,
                    proton_is_white,
                    time_control,
                    args.max_plies,
                    game_token,
                )
                records.append(estimate_elo.GameRecord(
                    pair=global_pair,
                    proton_color=proton_color,
                    opening_index=opening_index,
                    opening_fen=opening.fen,
                    opening_moves=list(opening.moves),
                    point=outcome.point,
                    termination=outcome.termination,
                    plies=outcome.plies,
                    moves=outcome.moves,
                    white_clock_seconds=outcome.white_clock_seconds,
                    black_clock_seconds=outcome.black_clock_seconds,
                    proton_seed=None,
                ))
                print(
                    f"opponent={level.label} pair={global_pair} "
                    f"proton={proton_color} result={outcome.point:g} "
                    f"termination={outcome.termination} "
                    f"score={sum(record.point for record in records):g}/"
                    f"{len(records)}",
                    flush=True,
                )
    finally:
        close_engine(proton)
        close_engine(stockfish)
    return summarize_records(level, records)


def main() -> int:
    args = parse_args()
    proton_path = args.proton.resolve()
    stockfish_path = args.stockfish.resolve()
    openings_path = args.openings.resolve()
    json_path = args.json_path.resolve()
    if not proton_path.is_file() or not stockfish_path.is_file():
        raise FileNotFoundError("both engine paths must be files")
    if not openings_path.is_file():
        raise FileNotFoundError(openings_path)
    if args.max_plies < 40 or args.hash < 1:
        raise ValueError("max-plies or hash is outside its valid range")

    time_control = build_time_control(args)
    elo_range = estimate_elo.inspect_uci_elo_range(stockfish_path)
    level = resolve_level(args.opponent_elo, args.full_strength, elo_range)
    openings = estimate_elo.load_openings(openings_path)
    selected = select_opening_shard(openings, args.pairs, args.offset, args.seed)
    result = run_shard(
        proton_path, stockfish_path, level, selected, args, time_control
    )

    tool_path = Path(__file__).resolve()
    match_core_path = Path(estimate_elo.__file__).resolve()
    proton_options = {
        "Hash": args.hash,
        "UseBook": False,
        "HumanStyle": False,
        "UCI_LimitStrength": False,
    }
    stockfish_options: dict[str, Any] = {
        "Hash": args.hash,
        "Threads": 1,
        "UCI_LimitStrength": level.limited,
    }
    if level.limited:
        stockfish_options["UCI_Elo"] = level.effective_elo

    created = datetime.now(timezone.utc).isoformat(timespec="seconds")
    report = CalibrationShardReport(
        schema_version=1,
        match_type="stockfish_fixed_node_calibration_shard",
        status="complete",
        created_utc=created,
        completed_utc=created,
        seed=args.seed,
        pair_offset=args.offset,
        pair_count=args.pairs,
        max_plies=args.max_plies,
        hash_mb=args.hash,
        time_control=time_control.payload(),
        proton=estimate_elo.identify_engine(proton_path),
        stockfish=estimate_elo.identify_engine(stockfish_path),
        stockfish_uci_elo_range=asdict(elo_range),
        opponent=asdict(level),
        proton_options=proton_options,
        stockfish_options=stockfish_options,
        openings=estimate_elo.InputArtifact(
            path=str(openings_path),
            sha256=estimate_elo.sha256_file(openings_path),
        ),
        opening_order="Python random.Random(seed).shuffle over indexed unique suite",
        selected_opening_indices=[index for index, _ in selected],
        tool=estimate_elo.InputArtifact(
            path=str(tool_path), sha256=estimate_elo.sha256_file(tool_path)
        ),
        match_core=estimate_elo.InputArtifact(
            path=str(match_core_path),
            sha256=estimate_elo.sha256_file(match_core_path),
        ),
        proton_source_commit=args.proton_commit,
        stockfish_source_ref=args.stockfish_ref,
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
        engine_lifecycle=(
            "one process pair for this shard; ucinewgame and isready before every "
            "game; shards are capped at 50 opening pairs"
        ),
        result=result,
    )
    estimate_elo.write_json_atomic(json_path, asdict(report))
    print(json.dumps(asdict(report), indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (chess.engine.EngineError, chess.engine.EngineTerminatedError) as error:
        print(f"engine failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
