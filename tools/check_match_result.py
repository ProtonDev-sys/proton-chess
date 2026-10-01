#!/usr/bin/env python3
"""Rebuild and check a match report against its pinned protocol and pass gate."""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import estimate_elo  # noqa: E402
from tools.run_match_protocol import load_protocol, sha256_file  # noqa: E402


def same_number(actual: Any, expected: float) -> bool:
    return (
        isinstance(actual, (int, float))
        and not isinstance(actual, bool)
        and math.isfinite(float(actual))
        and math.isclose(float(actual), expected, rel_tol=0.0, abs_tol=1e-12)
    )


def same_hash(actual: Any, expected: str) -> bool:
    return isinstance(actual, str) and actual.lower() == expected.lower()


def expected_point(record: estimate_elo.GameRecord, winner: chess.Color | None) -> float:
    if winner is None:
        return 0.5
    proton_is_white = record.proton_color == "white"
    return 1.0 if winner == proton_is_white else 0.0


def validate_record_replay(
    record: estimate_elo.GameRecord,
    max_plies: int,
) -> list[str]:
    label = f"pair {record.pair} Proton {record.proton_color}"
    errors: list[str] = []
    try:
        board = chess.Board(record.opening_fen)
    except (TypeError, ValueError):
        return [f"{label} has an invalid opening FEN"]
    if not board.is_valid() or board.is_game_over(claim_draw=True):
        errors.append(f"{label} opening is invalid or terminal")
        return errors

    for text in record.opening_moves:
        try:
            move = chess.Move.from_uci(text)
        except ValueError:
            errors.append(f"{label} has malformed opening move {text!r}")
            return errors
        if move not in board.legal_moves:
            errors.append(f"{label} has illegal opening move {text}")
            return errors
        board.push(move)

    if record.moves[:len(record.opening_moves)] != record.opening_moves:
        errors.append(f"{label} raw moves do not begin with the recorded opening moves")
        return errors
    for text in record.moves[len(record.opening_moves):]:
        if board.is_game_over(claim_draw=True):
            errors.append(f"{label} contains a move after the game had ended")
            return errors
        if board.ply() >= max_plies:
            errors.append(f"{label} contains a move after reaching max_plies")
            return errors
        try:
            move = chess.Move.from_uci(text)
        except ValueError:
            errors.append(f"{label} has malformed move {text!r}")
            return errors
        if move not in board.legal_moves:
            errors.append(f"{label} has illegal move {text}")
            return errors
        board.push(move)

    if record.plies != board.ply():
        errors.append(f"{label} reported ply count does not match replay")
    outcome = board.outcome(claim_draw=True)
    if outcome is not None:
        termination = outcome.termination.name.lower().replace("_", " ")
        if record.termination != termination:
            errors.append(f"{label} termination does not match replayed board outcome")
        if not same_number(record.point, expected_point(record, outcome.winner)):
            errors.append(f"{label} point does not match replayed board outcome")
        return errors

    if record.termination == "move limit":
        if board.ply() < max_plies:
            errors.append(f"{label} claims the move limit before max_plies")
        if not same_number(record.point, 0.5):
            errors.append(f"{label} move-limit result is not a draw")
        return errors

    failure_terminations = {
        "white flag": (True, "flag"),
        "black flag": (False, "flag"),
        "white timeout": (True, "timeout"),
        "black timeout": (False, "timeout"),
        "white illegal or no move": (True, "illegal"),
        "black illegal or no move": (False, "illegal"),
    }
    failure = failure_terminations.get(record.termination)
    if failure is not None:
        failed_white, failure_kind = failure
        if board.ply() >= max_plies:
            errors.append(f"{label} failure occurs at or beyond the move limit")
        if board.turn != failed_white:
            errors.append(f"{label} failure side is not the side to move")
        proton_is_white = record.proton_color == "white"
        expected = 0.0 if failed_white == proton_is_white else 1.0
        if not same_number(record.point, expected):
            errors.append(f"{label} point does not match the failed side")
        if failure_kind in {"flag", "timeout"}:
            failed_clock = (
                record.white_clock_seconds if failed_white else record.black_clock_seconds
            )
            if failed_clock != 0.0:
                errors.append(f"{label} failed clock is not zero")
        return errors

    errors.append(f"{label} termination cannot be reproduced")
    return errors


def parse_records(
    raw_records: Any,
    rating: int,
    protocol: dict[str, Any],
    openings: list[estimate_elo.OpeningPosition],
) -> tuple[list[estimate_elo.GameRecord], list[str]]:
    errors: list[str] = []
    if not isinstance(raw_records, list):
        return [], [f"Stockfish Elo {rating} records are not a list"]
    expected_games = protocol["games_per_level"]
    if len(raw_records) != expected_games:
        errors.append(
            f"Stockfish Elo {rating} has {len(raw_records)} raw records, "
            f"expected exactly {expected_games}"
        )

    records: list[estimate_elo.GameRecord] = []
    for index, payload in enumerate(raw_records, start=1):
        if not isinstance(payload, dict):
            errors.append(f"Stockfish Elo {rating} record {index} is not an object")
            continue
        try:
            record = estimate_elo.GameRecord(**payload)
        except TypeError as error:
            errors.append(f"Stockfish Elo {rating} record {index} has wrong fields: {error}")
            continue
        if not isinstance(record.pair, int) or isinstance(record.pair, bool):
            errors.append(f"Stockfish Elo {rating} record {index} has invalid pair number")
            continue
        if record.proton_color not in {"white", "black"}:
            errors.append(f"Stockfish Elo {rating} record {index} has invalid Proton color")
            continue
        if isinstance(record.point, bool) or record.point not in {0.0, 0.5, 1.0}:
            errors.append(f"Stockfish Elo {rating} record {index} has invalid point")
            continue
        if not isinstance(record.opening_moves, list) or not isinstance(record.moves, list):
            errors.append(f"Stockfish Elo {rating} record {index} has invalid move lists")
            continue
        if not all(isinstance(move, str) for move in record.opening_moves + record.moves):
            errors.append(f"Stockfish Elo {rating} record {index} has non-string moves")
            continue
        if not isinstance(record.termination, str) or not isinstance(record.plies, int):
            errors.append(f"Stockfish Elo {rating} record {index} has invalid outcome fields")
            continue
        if "proton_seed" in payload and record.proton_seed is not None:
            errors.append(
                f"Stockfish Elo {rating} record {index} has a Proton limiter seed"
            )
        for clock_name, clock in (
            ("white", record.white_clock_seconds),
            ("black", record.black_clock_seconds),
        ):
            if clock is None:
                errors.append(
                    f"Stockfish Elo {rating} record {index} has no {clock_name} Fischer clock"
                )
            elif (
                not isinstance(clock, (int, float))
                or isinstance(clock, bool)
                or not math.isfinite(float(clock))
                or clock < 0.0
            ):
                errors.append(
                    f"Stockfish Elo {rating} record {index} has invalid {clock_name} clock"
                )
        records.append(record)

    if len(raw_records) != expected_games:
        return records, errors

    pair_count = expected_games // 2
    schedule = estimate_elo.select_openings(
        openings, pair_count, protocol["seed"] + rating
    )
    grouped: dict[int, list[estimate_elo.GameRecord]] = {}
    for record in records:
        grouped.setdefault(record.pair, []).append(record)
    if set(grouped) != set(range(1, pair_count + 1)):
        errors.append(f"Stockfish Elo {rating} pair numbers do not cover 1..{pair_count}")
    for pair, (opening_index, opening) in enumerate(schedule, start=1):
        pair_records = grouped.get(pair, [])
        if len(pair_records) != 2:
            errors.append(f"Stockfish Elo {rating} pair {pair} does not contain two games")
            continue
        if {record.proton_color for record in pair_records} != {"white", "black"}:
            errors.append(f"Stockfish Elo {rating} pair {pair} does not swap Proton colors")
        for record in pair_records:
            if (
                record.opening_index != opening_index
                or record.opening_fen != opening.fen
                or record.opening_moves != opening.moves
            ):
                errors.append(
                    f"Stockfish Elo {rating} pair {pair} does not match its pinned opening"
                )
            errors.extend(validate_record_replay(record, protocol["max_plies"]))
    return records, errors


def compare_summary(
    reported: dict[str, Any],
    computed: estimate_elo.MatchResult,
) -> list[str]:
    rating = computed.opponent_elo
    errors: list[str] = []
    exact_fields = (
        "opponent_elo", "games", "wins", "draws", "losses", "pair_count",
        "pair_scores", "pentanomial_counts", "confidence_method",
        "significant_above_50",
    )
    for field in exact_fields:
        if reported.get(field) != getattr(computed, field):
            errors.append(f"Stockfish Elo {rating} reported {field} does not recompute")
    float_fields = (
        "score", "estimated_elo", "score_ci95_low", "score_ci95_high",
    )
    for field in float_fields:
        if not same_number(reported.get(field), getattr(computed, field)):
            errors.append(f"Stockfish Elo {rating} reported {field} does not recompute")
    if computed.complete_pair_score is None:
        if reported.get("complete_pair_score") is not None:
            errors.append(
                f"Stockfish Elo {rating} reported complete_pair_score does not recompute"
            )
    elif not same_number(
        reported.get("complete_pair_score"), computed.complete_pair_score
    ):
        errors.append(
            f"Stockfish Elo {rating} reported complete_pair_score does not recompute"
        )
    return errors


def load_verified_openings(
    protocol: dict[str, Any], repository_root: Path
) -> list[estimate_elo.OpeningPosition]:
    runner = (repository_root / protocol["runner"]["path"]).resolve()
    openings_path = (repository_root / protocol["openings"]["path"]).resolve()
    if sha256_file(runner) != protocol["runner"]["sha256"].lower():
        raise ValueError("local runner hash does not match the protocol")
    if sha256_file(openings_path) != protocol["openings"]["sha256"].lower():
        raise ValueError("local opening suite hash does not match the protocol")
    if getattr(chess, "__version__", "unknown") != protocol["python_chess_version"]:
        raise ValueError("local python-chess version does not match the protocol")
    openings = estimate_elo.load_openings(openings_path)
    if len(openings) != protocol["openings"]["positions"]:
        raise ValueError("local opening suite position count does not match the protocol")
    if len({(item.fen, tuple(item.moves)) for item in openings}) != len(openings):
        raise ValueError("local opening suite contains duplicate normalized positions")
    return openings


def validate_result(
    protocol: dict[str, Any],
    report: dict[str, Any],
    openings: list[estimate_elo.OpeningPosition],
) -> list[str]:
    errors: list[str] = []

    def require(condition: bool, message: str) -> None:
        if not condition:
            errors.append(message)

    require(report.get("schema_version") == 2, "report schema_version is not 2")
    require(report.get("status") == "complete", "report status is not complete")
    require(report.get("completed_utc") is not None, "report has no completion timestamp")
    require(
        report.get("requested_games_per_level") == protocol["games_per_level"],
        "requested game count does not match the protocol",
    )
    require(report.get("max_plies") == protocol["max_plies"], "max_plies mismatch")
    require(report.get("hash_mb") == protocol["hash_mb"], "hash size mismatch")
    require(report.get("seed") == protocol["seed"], "seed mismatch")
    require(
        same_hash(report.get("tool", {}).get("sha256"), protocol["runner"]["sha256"]),
        "runner hash mismatch",
    )
    require(
        report.get("python_chess_version") == protocol["python_chess_version"],
        "python-chess version mismatch",
    )
    require(
        report.get("host", {}).get("system") == protocol["platform"]["system"],
        "host system mismatch",
    )
    require(
        str(report.get("host", {}).get("machine", "")).lower()
        == protocol["platform"]["machine"].lower(),
        "host machine mismatch",
    )
    if protocol.get("require_clean_git"):
        require(report.get("git_dirty") is False, "report was produced from a dirty tree")

    expected_time = protocol["time_control"]
    actual_time = report.get("time_control", {})
    require(actual_time.get("mode") == "fischer", "time-control mode is not Fischer")
    require(
        actual_time.get("base_seconds") == expected_time["base_seconds"],
        "base clock mismatch",
    )
    require(
        actual_time.get("increment_seconds") == expected_time["increment_seconds"],
        "increment mismatch",
    )
    require(
        actual_time.get("watchdog_grace_seconds")
        == expected_time["watchdog_grace_seconds"],
        "watchdog grace mismatch",
    )
    require(
        same_hash(
            report.get("openings", {}).get("sha256"),
            protocol["openings"]["sha256"],
        ),
        "opening suite hash mismatch",
    )
    require(
        same_hash(
            report.get("stockfish", {}).get("sha256"),
            protocol["stockfish"]["binary_sha256"],
        ),
        "Stockfish binary hash mismatch",
    )
    require(
        same_hash(
            report.get("proton", {}).get("sha256"),
            protocol["proton"]["binary_sha256"],
        ),
        "Proton binary hash mismatch",
    )
    require(
        report.get("proton_options") == protocol["proton_options"],
        "Proton options mismatch",
    )
    expected_stockfish_options = protocol["stockfish"]["options"]
    actual_stockfish_options = report.get("stockfish_options", {})
    require(
        actual_stockfish_options.get("Hash") == expected_stockfish_options["Hash"],
        "Stockfish Hash mismatch",
    )
    require(
        actual_stockfish_options.get("Threads") == expected_stockfish_options["Threads"],
        "Stockfish Threads mismatch",
    )
    require(
        actual_stockfish_options.get("UCI_LimitStrength")
        == expected_stockfish_options["UCI_LimitStrength"],
        "Stockfish UCI_LimitStrength mismatch",
    )
    require(
        actual_stockfish_options.get("UCI_Elo") == protocol["opponent_elo"],
        "Stockfish Elo list mismatch",
    )

    if "proton_target_elo" in report:
        require(
            report.get("proton_target_elo") is None,
            "certification report enables a Proton Elo target",
        )
    if "proton_seed_derivation" in report:
        require(
            report.get("proton_seed_derivation") is None,
            "certification report declares Proton seed derivation",
        )
    if "stockfish_requested_elo" in report:
        require(
            report.get("stockfish_requested_elo") == protocol["opponent_elo"],
            "requested Stockfish Elo metadata mismatch",
        )
    if "stockfish_effective_elo" in report:
        require(
            report.get("stockfish_effective_elo") == protocol["opponent_elo"],
            "effective Stockfish Elo metadata mismatch",
        )
    if "proton_uci_elo_range" in report:
        require(
            report.get("proton_uci_elo_range") is None,
            "certification report contains a limited-Proton Elo range",
        )
    if "stockfish_uci_elo_range" in report:
        elo_range = report.get("stockfish_uci_elo_range")
        valid_range = isinstance(elo_range, dict) and set(elo_range) == {
            "minimum", "maximum", "default",
        }
        if valid_range:
            minimum = elo_range.get("minimum")
            maximum = elo_range.get("maximum")
            default = elo_range.get("default")
            valid_range = (
                isinstance(minimum, int)
                and not isinstance(minimum, bool)
                and isinstance(maximum, int)
                and not isinstance(maximum, bool)
                and minimum > 0
                and minimum <= maximum
                and all(minimum <= rating <= maximum for rating in protocol["opponent_elo"])
                and (
                    default is None
                    or (
                        isinstance(default, int)
                        and not isinstance(default, bool)
                        and minimum <= default <= maximum
                    )
                )
            )
        require(valid_range, "Stockfish UCI Elo range metadata is invalid")

    results = report.get("results", [])
    if not isinstance(results, list):
        return [*errors, "results are not a list"]
    ratings = [result.get("opponent_elo") for result in results if isinstance(result, dict)]
    require(ratings == protocol["opponent_elo"], "result Elo levels or order mismatch")
    gate = protocol["pass_condition"]
    for rating, result in zip(protocol["opponent_elo"], results, strict=False):
        if not isinstance(result, dict):
            errors.append(f"Stockfish Elo {rating} result is not an object")
            continue
        if "opponent_elo_requested" in result:
            require(
                result.get("opponent_elo_requested") == rating,
                f"Stockfish Elo {rating} requested-Elo metadata mismatch",
            )
        records, record_errors = parse_records(
            result.get("records"), rating, protocol, openings
        )
        errors.extend(record_errors)
        if len(records) != protocol["games_per_level"]:
            continue
        try:
            computed = estimate_elo.summarize_level(rating, records)
        except (TypeError, ValueError) as error:
            errors.append(f"Stockfish Elo {rating} records cannot be summarized: {error}")
            continue
        errors.extend(compare_summary(result, computed))
        require(
            computed.games == gate["minimum_games"],
            f"Stockfish Elo {rating} does not have exactly {gate['minimum_games']} games",
        )
        threshold = gate["score_ci95_low_strictly_greater_than"]
        require(
            computed.score_ci95_low > threshold,
            f"Stockfish Elo {rating} recomputed score lower bound does not exceed {threshold}",
        )
        require(
            computed.significant_above_50,
            f"Stockfish Elo {rating} recomputed significance flag is false",
        )
    return errors


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Rebuild and validate a schema-v2 report against a pinned protocol."
    )
    parser.add_argument("protocol", type=Path)
    parser.add_argument("report", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        protocol_path = args.protocol.resolve()
        protocol = load_protocol(protocol_path)
        openings = load_verified_openings(protocol, ROOT)
        report = json.loads(args.report.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    errors = validate_result(protocol, report, openings)
    if errors:
        for error in errors:
            print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print("PASS: raw records match the protocol and clear the recomputed score gate")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
