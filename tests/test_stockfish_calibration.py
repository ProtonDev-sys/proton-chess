#!/usr/bin/env python3
"""Focused tests for fixed-node Stockfish calibration shards."""

from __future__ import annotations

import contextlib
import io
import json
import sys
import tempfile
import unittest
from argparse import Namespace
from dataclasses import asdict
from pathlib import Path
from unittest import mock

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import calibrate_stockfish  # noqa: E402
from tools import estimate_elo  # noqa: E402
from tools import merge_stockfish_calibrations  # noqa: E402


class RecordingEngine:
    def __init__(self, source: str = "engine") -> None:
        self.source = source
        self.configurations: list[dict[str, object]] = []
        self.closed = False

    def configure(self, configuration: dict[str, object]) -> None:
        self.configurations.append(dict(configuration))

    def close(self) -> None:
        self.closed = True


class StockfishCalibrationTests(unittest.TestCase):
    @staticmethod
    def record(pair: int, color: str, point: float, opening: int | None = None) -> estimate_elo.GameRecord:
        return estimate_elo.GameRecord(
            pair=pair,
            proton_color=color,
            opening_index=pair if opening is None else opening,
            opening_fen=chess.STARTING_FEN,
            opening_moves=[],
            point=point,
            termination="test",
            plies=1,
            moves=["e2e4"],
            white_clock_seconds=None,
            black_clock_seconds=None,
            proton_seed=None,
        )

    def test_fixed_node_control_and_shard_selection(self) -> None:
        args = Namespace(nodes=4096, watchdog_grace=3.0)
        control = calibrate_stockfish.build_time_control(args)
        self.assertEqual(control.limit(None).nodes, 4096)
        self.assertEqual(control.watchdog(chess.WHITE, None), 3.0)
        self.assertEqual(control.payload()["mode"], "fixed_nodes")

        openings = [
            estimate_elo.OpeningPosition(chess.STARTING_FEN, [str(index)])
            for index in range(80)
        ]
        first = calibrate_stockfish.select_opening_shard(openings, 10, 0, 17)
        second = calibrate_stockfish.select_opening_shard(openings, 10, 10, 17)
        self.assertFalse({index for index, _ in first} & {index for index, _ in second})
        self.assertEqual(first, calibrate_stockfish.select_opening_shard(openings, 10, 0, 17))
        with self.assertRaisesRegex(ValueError, "pairs must be between"):
            calibrate_stockfish.select_opening_shard(openings, 51, 0, 17)
        with self.assertRaisesRegex(ValueError, "exceeds"):
            calibrate_stockfish.select_opening_shard(openings, 10, 75, 17)

    def test_level_resolution_and_configuration(self) -> None:
        elo_range = estimate_elo.UciEloRange(1320, 3190, 1320)
        limited = calibrate_stockfish.resolve_level(1000, False, elo_range)
        self.assertEqual((limited.requested_elo, limited.effective_elo), (1000, 1320))
        full = calibrate_stockfish.resolve_level(None, True, elo_range)
        self.assertFalse(full.limited)

        proton = RecordingEngine()
        stockfish = RecordingEngine()
        _, limited_options = calibrate_stockfish.configure_engines(
            proton, stockfish, limited, 16
        )
        self.assertTrue(limited_options["UCI_LimitStrength"])
        self.assertEqual(limited_options["UCI_Elo"], 1320)
        _, full_options = calibrate_stockfish.configure_engines(
            proton, stockfish, full, 16
        )
        self.assertFalse(full_options["UCI_LimitStrength"])
        self.assertNotIn("UCI_Elo", full_options)

    def test_summary_uses_opening_pairs(self) -> None:
        records = [
            self.record(pair, color, 1.0)
            for pair in range(1, 201)
            for color in ("white", "black")
        ]
        result = calibrate_stockfish.summarize_records(
            calibrate_stockfish.OpponentLevel("1700", 1700, 1700, True),
            records,
        )
        self.assertTrue(result.significant_above_50)
        self.assertTrue(result.observed_score_at_least_70)
        self.assertEqual(result.pair_count, 200)
        self.assertEqual(result.pentanomial_counts["2.0"], 200)

    def test_run_shard_reuses_one_process_pair_and_swaps_colors(self) -> None:
        args = Namespace(seed=17, hash=16, max_plies=40, offset=50)
        level = calibrate_stockfish.OpponentLevel("1700", 1700, 1700, True)
        selected = [
            (4, estimate_elo.OpeningPosition(chess.STARTING_FEN, ["e2e4"])),
            (9, estimate_elo.OpeningPosition(chess.STARTING_FEN, ["d2d4"])),
        ]
        outcomes = [
            estimate_elo.GameOutcome(point, "test", 1, ["e2e4"], None, None)
            for point in (1.0, 0.5, 0.0, 0.5)
        ]
        engines: list[RecordingEngine] = []

        def open_engine(path: str) -> RecordingEngine:
            engine = RecordingEngine(path)
            engines.append(engine)
            return engine

        with (
            mock.patch.object(
                calibrate_stockfish.chess.engine.SimpleEngine,
                "popen_uci",
                side_effect=open_engine,
            ),
            mock.patch.object(calibrate_stockfish.estimate_elo, "prepare_engine_for_game")
            as prepare,
            mock.patch.object(
                calibrate_stockfish.estimate_elo,
                "play_game",
                side_effect=outcomes,
            ) as play_game,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = calibrate_stockfish.run_shard(
                Path("proton"),
                Path("stockfish"),
                level,
                selected,
                args,
                calibrate_stockfish.FixedNodeTimeControl(1000, 2.0),
            )

        self.assertEqual(len(engines), 2)
        self.assertTrue(all(engine.closed for engine in engines))
        self.assertEqual(prepare.call_count, 8)
        self.assertEqual(play_game.call_count, 4)
        self.assertEqual(
            [record.pair for record in result.records], [51, 51, 52, 52]
        )
        self.assertEqual(
            [record.proton_color for record in result.records],
            ["white", "black", "white", "black"],
        )

    def test_merge_rejects_overlap_and_combines_disjoint_shards(self) -> None:
        level = calibrate_stockfish.OpponentLevel("1700", 1700, 1700, True)

        def report(offset: int, opening: int, points: tuple[float, float]) -> dict[str, object]:
            records = [
                self.record(offset + 1, "white", points[0], opening),
                self.record(offset + 1, "black", points[1], opening),
            ]
            result = calibrate_stockfish.summarize_records(level, records)
            return {
                "schema_version": 1,
                "match_type": "stockfish_fixed_node_calibration_shard",
                "status": "complete",
                "seed": 17,
                "pair_offset": offset,
                "pair_count": 1,
                "max_plies": 200,
                "hash_mb": 16,
                "time_control": {"mode": "fixed_nodes", "nodes_per_move": 1000},
                "proton": {"sha256": "1" * 64},
                "stockfish": {"sha256": "2" * 64},
                "stockfish_uci_elo_range": {"minimum": 1320, "maximum": 3190, "default": 1320},
                "opponent": asdict(level),
                "proton_options": {"Hash": 16},
                "stockfish_options": {"Hash": 16, "UCI_Elo": 1700},
                "openings": {"sha256": "3" * 64},
                "opening_order": "test",
                "selected_opening_indices": [opening],
                "tool": {"sha256": "4" * 64},
                "match_core": {"sha256": "5" * 64},
                "proton_source_commit": "abc",
                "stockfish_source_ref": "sf_18",
                "result": asdict(result),
            }

        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            first = root / "first.json"
            second = root / "second.json"
            first.write_text(json.dumps(report(0, 10, (1.0, 1.0))), encoding="utf-8")
            second.write_text(json.dumps(report(1, 11, (1.0, 0.5))), encoding="utf-8")
            merged = merge_stockfish_calibrations.merge_shards([first, second])
            self.assertEqual(merged["pair_count"], 2)
            self.assertEqual(merged["games"], 4)
            self.assertEqual(merged["result"]["score"], 0.875)

            second.write_text(json.dumps(report(0, 10, (1.0, 0.5))), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "overlap"):
                merge_stockfish_calibrations.merge_shards([first, second])


if __name__ == "__main__":
    unittest.main()
