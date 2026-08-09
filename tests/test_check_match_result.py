#!/usr/bin/env python3
"""Focused tests for the raw-record certification result gate."""

from __future__ import annotations

import platform
import sys
import unittest
from dataclasses import asdict
from pathlib import Path

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import check_match_result, estimate_elo  # noqa: E402


class CheckMatchResultTests(unittest.TestCase):
    @staticmethod
    def openings() -> list[estimate_elo.OpeningPosition]:
        return [
            estimate_elo.OpeningPosition(
                f"{chess.STARTING_BOARD_FEN} w KQkq - 0 {index + 1}", []
            )
            for index in range(200)
        ]

    @staticmethod
    def protocol() -> dict[str, object]:
        return {
            "schema_version": 1,
            "runner": {"path": "tools/estimate_elo.py", "sha256": "runner"},
            "python_chess_version": chess.__version__,
            "platform": {"system": platform.system(), "machine": platform.machine()},
            "require_clean_git": True,
            "games_per_level": 400,
            "opponent_elo": [3000],
            "max_plies": 600,
            "hash_mb": 64,
            "seed": 20260809,
            "time_control": {
                "base_seconds": 60.0,
                "increment_seconds": 0.6,
                "watchdog_grace_seconds": 10.0,
            },
            "openings": {"sha256": "openings"},
            "stockfish": {
                "binary_sha256": "stockfish",
                "options": {
                    "Hash": 64,
                    "Threads": 1,
                    "UCI_LimitStrength": True,
                    "UCI_Elo": 3000,
                },
            },
            "proton": {"binary_sha256": "proton"},
            "proton_options": {"Hash": 64, "UseBook": False, "HumanStyle": False},
            "pass_condition": {
                "minimum_games": 400,
                "score_ci95_low_strictly_greater_than": 0.5,
            },
        }

    def passing_report(self) -> dict[str, object]:
        openings = self.openings()
        schedule = estimate_elo.select_openings(openings, 200, 20260809 + 3000)
        records: list[estimate_elo.GameRecord] = []
        for pair, (opening_index, opening) in enumerate(schedule, start=1):
            start_ply = chess.Board(opening.fen).ply()
            records.extend((
                estimate_elo.GameRecord(
                    pair, "white", opening_index, opening.fen, [], 1.0,
                    "black flag", start_ply + 1, ["e2e4"], 60.0, 0.0,
                ),
                estimate_elo.GameRecord(
                    pair, "black", opening_index, opening.fen, [], 1.0,
                    "white flag", start_ply, [], 0.0, 60.0,
                ),
            ))
        summary = asdict(estimate_elo.summarize_level(3000, records))
        return {
            "schema_version": 2,
            "status": "complete",
            "completed_utc": "2026-08-09T00:00:00+00:00",
            "requested_games_per_level": 400,
            "max_plies": 600,
            "hash_mb": 64,
            "seed": 20260809,
            "git_dirty": False,
            "python_chess_version": chess.__version__,
            "host": {"system": platform.system(), "machine": platform.machine()},
            "time_control": {
                "mode": "fischer",
                "base_seconds": 60.0,
                "increment_seconds": 0.6,
                "watchdog_grace_seconds": 10.0,
            },
            "tool": {"sha256": "runner"},
            "openings": {"sha256": "openings"},
            "stockfish": {"sha256": "stockfish"},
            "proton": {"sha256": "proton"},
            "proton_options": {"Hash": 64, "UseBook": False, "HumanStyle": False},
            "stockfish_options": {
                "Hash": 64,
                "Threads": 1,
                "UCI_LimitStrength": True,
                "UCI_Elo": [3000],
            },
            "results": [summary],
        }

    def test_complete_matching_raw_report_clears_gate(self) -> None:
        self.assertEqual(
            check_match_result.validate_result(
                self.protocol(), self.passing_report(), self.openings()
            ),
            [],
        )

    def test_fabricated_summary_is_rejected_after_recomputation(self) -> None:
        report = self.passing_report()
        report["results"][0]["score_ci95_low"] = 0.999
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn(
            "Stockfish Elo 3000 reported score_ci95_low does not recompute", errors
        )

    def test_missing_records_are_rejected_without_trusting_summary(self) -> None:
        report = self.passing_report()
        report["results"][0]["records"] = report["results"][0]["records"][:-1]
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn(
            "Stockfish Elo 3000 has 399 raw records, expected exactly 400", errors
        )

    def test_incomplete_pair_schedule_is_rejected(self) -> None:
        report = self.passing_report()
        report["results"][0]["records"][-1]["pair"] = 199
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn("Stockfish Elo 3000 pair 199 does not contain two games", errors)
        self.assertIn("Stockfish Elo 3000 pair 200 does not contain two games", errors)

    def test_wrong_pinned_opening_is_rejected(self) -> None:
        report = self.passing_report()
        report["results"][0]["records"][0]["opening_index"] = 999
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertTrue(any("does not match its pinned opening" in error for error in errors))

    def test_illegal_move_and_non_finite_summary_are_rejected(self) -> None:
        report = self.passing_report()
        report["results"][0]["records"][0]["moves"] = ["e2e5"]
        report["results"][0]["score"] = float("nan")
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertTrue(any("has illegal move e2e5" in error for error in errors))
        self.assertIn("Stockfish Elo 3000 reported score does not recompute", errors)

    def test_boolean_point_is_rejected(self) -> None:
        report = self.passing_report()
        report["results"][0]["records"][0]["point"] = True
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn("Stockfish Elo 3000 record 1 has invalid point", errors)

    def test_hash_comparisons_are_case_insensitive(self) -> None:
        protocol = self.protocol()
        protocol["runner"]["sha256"] = "RUNNER"
        protocol["openings"]["sha256"] = "OPENINGS"
        protocol["stockfish"]["binary_sha256"] = "STOCKFISH"
        protocol["proton"]["binary_sha256"] = "PROTON"
        self.assertEqual(
            check_match_result.validate_result(
                protocol, self.passing_report(), self.openings()
            ),
            [],
        )

    def test_proton_report_hash_mismatch_is_rejected(self) -> None:
        report = self.passing_report()
        report["proton"]["sha256"] = "different"
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn("Proton binary hash mismatch", errors)

    def test_dirty_tree_and_dependency_drift_are_rejected(self) -> None:
        report = self.passing_report()
        report["git_dirty"] = True
        report["python_chess_version"] = "different"
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertIn("report was produced from a dirty tree", errors)
        self.assertIn("python-chess version mismatch", errors)

    def test_fischer_records_require_both_clocks_and_exact_failure_text(self) -> None:
        report = self.passing_report()
        record = report["results"][0]["records"][0]
        record["white_clock_seconds"] = None
        record["termination"] = "black illegal fabricated"
        errors = check_match_result.validate_result(
            self.protocol(), report, self.openings()
        )
        self.assertTrue(any("has no white Fischer clock" in error for error in errors))
        self.assertTrue(any("termination cannot be reproduced" in error for error in errors))

    def test_failure_cannot_override_the_move_limit(self) -> None:
        fen = f"{chess.STARTING_BOARD_FEN} w KQkq - 0 301"
        record = estimate_elo.GameRecord(
            1, "black", 0, fen, [], 1.0, "white flag", 600, [], 0.0, 60.0
        )
        errors = check_match_result.validate_record_replay(record, 600)
        self.assertIn(
            "pair 1 Proton black failure occurs at or beyond the move limit", errors
        )

    def test_replay_rejects_move_after_claimable_threefold(self) -> None:
        moves = [
            "g1f3", "g8f6", "f3g1", "f6g8",
            "g1f3", "g8f6", "f3g1", "f6g8",
            "e2e4",
        ]
        record = estimate_elo.GameRecord(
            1, "white", 0, chess.STARTING_FEN, [], 1.0,
            "black illegal or no move", 9, moves, 60.0, 60.0,
        )
        errors = check_match_result.validate_record_replay(record, 600)
        self.assertIn("pair 1 Proton white contains a move after the game had ended", errors)

    def test_replay_rejects_move_after_ply_limit(self) -> None:
        record = estimate_elo.GameRecord(
            1, "black", 0, chess.STARTING_FEN, [], 0.0,
            "white illegal or no move", 2, ["e2e4", "e7e5"], 60.0, 60.0,
        )
        errors = check_match_result.validate_record_replay(record, 1)
        self.assertIn("pair 1 Proton black contains a move after reaching max_plies", errors)


if __name__ == "__main__":
    unittest.main()
