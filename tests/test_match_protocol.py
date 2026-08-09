#!/usr/bin/env python3
"""Focused tests for pinned match protocol verification."""

from __future__ import annotations

import json
import platform
import sys
import tempfile
import unittest
from pathlib import Path

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import run_match_protocol  # noqa: E402


class MatchProtocolTests(unittest.TestCase):
    @staticmethod
    def fixture(root: Path) -> tuple[Path, Path, Path, Path]:
        (root / "tools").mkdir()
        (root / "openings").mkdir()
        (root / "matches").mkdir()
        runner = root / "tools" / "estimate_elo.py"
        openings = root / "openings" / "suite.epd"
        proton = root / "proton.exe"
        stockfish = root / "stockfish.exe"
        runner.write_text("# test\n", encoding="utf-8")
        openings.write_text(
            "".join(
                f"{chess.STARTING_BOARD_FEN} w KQkq - 0 {index + 1}\n"
                for index in range(200)
            ),
            encoding="utf-8",
        )
        proton.write_bytes(b"proton")
        stockfish.write_bytes(b"stockfish")
        protocol = {
            "schema_version": 1,
            "name": "test-protocol",
            "runner": {
                "path": "tools/estimate_elo.py",
                "sha256": run_match_protocol.sha256_file(runner),
            },
            "python_chess_version": chess.__version__,
            "platform": {
                "system": platform.system(),
                "machine": platform.machine(),
            },
            "require_clean_git": False,
            "games_per_level": 400,
            "opponent_elo": [3000],
            "time_control": {
                "base_seconds": 60.0,
                "increment_seconds": 0.6,
                "watchdog_grace_seconds": 10.0,
            },
            "max_plies": 600,
            "hash_mb": 64,
            "seed": 20260809,
            "pairing": "same opening twice with colors swapped",
            "adjudication": "legal results and claimable draws",
            "openings": {
                "path": "openings/suite.epd",
                "sha256": run_match_protocol.sha256_file(openings),
                "positions": 200,
                "source_repository": "https://example.test/books",
                "source_commit": "0" * 40,
                "source_archive": "suite.zip",
                "source_archive_sha256": "1" * 64,
                "source_epd_sha256": "2" * 64,
                "license": "CC0-1.0",
            },
            "stockfish": {
                "name": "Stockfish",
                "release_tag": "test",
                "release_url": "https://example.test/stockfish",
                "asset_name": "stockfish.zip",
                "asset_sha256": "3" * 64,
                "binary_name": "stockfish.exe",
                "binary_sha256": run_match_protocol.sha256_file(stockfish),
                "options": {
                    "Hash": 64,
                    "Threads": 1,
                    "UCI_LimitStrength": True,
                    "UCI_Elo": 3000,
                },
            },
            "proton": {
                "name": "Proton Chess",
                "binary_sha256": run_match_protocol.sha256_file(proton),
            },
            "proton_options": {"Hash": 64, "UseBook": False, "HumanStyle": False},
            "pass_condition": {
                "minimum_games": 400,
                "score_ci95_low_strictly_greater_than": 0.5,
            },
        }
        protocol_path = root / "matches" / "protocol.json"
        protocol_path.write_text(json.dumps(protocol), encoding="utf-8")
        return protocol_path, proton, stockfish, openings

    def test_build_command_contains_the_exact_protocol(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, openings = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            with run_match_protocol.staged_inputs(
                protocol, root, proton, stockfish
            ) as (runner, staged_openings, staged_proton, staged_stockfish):
                command = run_match_protocol.build_command(
                    protocol,
                    runner,
                    staged_openings,
                    staged_proton,
                    staged_stockfish,
                    root / "result.json",
                )
                stage_root = runner.parents[1]
                self.assertNotEqual(runner, (root / "tools" / "estimate_elo.py").resolve())
                self.assertNotEqual(staged_proton, proton.resolve())
                self.assertNotEqual(staged_stockfish, stockfish.resolve())
                self.assertEqual(Path(command[1]), runner)
                self.assertEqual(Path(command[2]), staged_proton)
                self.assertEqual(Path(command[3]), staged_stockfish)
                self.assertEqual(
                    run_match_protocol.sha256_file(runner),
                    protocol["runner"]["sha256"],
                )
                self.assertEqual(
                    run_match_protocol.sha256_file(staged_openings),
                    protocol["openings"]["sha256"],
                )
                expected_pairs = {
                    "--opponent-elo": "3000",
                    "--games": "400",
                    "--base-seconds": "60.0",
                    "--increment": "0.6",
                    "--watchdog-grace": "10.0",
                    "--max-plies": "600",
                    "--hash": "64",
                    "--seed": "20260809",
                    "--openings": str(staged_openings),
                    "--json": str((root / "result.json").resolve()),
                }
                for flag, value in expected_pairs.items():
                    index = command.index(flag)
                    self.assertEqual(command[index + 1], value)
            self.assertFalse(stage_root.exists())

    def test_stockfish_hash_mismatch_stops_before_play(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, _ = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            stockfish.write_bytes(b"different")
            with self.assertRaisesRegex(ValueError, "Stockfish SHA-256 mismatch"):
                run_match_protocol.verified_inputs(protocol, root, proton, stockfish)

    def test_proton_hash_mismatch_stops_before_play(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, _ = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            proton.write_bytes(b"different")
            with self.assertRaisesRegex(ValueError, "Proton SHA-256 mismatch"):
                run_match_protocol.verified_inputs(protocol, root, proton, stockfish)

    def test_opening_hash_mismatch_stops_before_play(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, openings = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            openings.write_text("changed\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "opening suite SHA-256 mismatch"):
                run_match_protocol.verified_inputs(protocol, root, proton, stockfish)

    def test_odd_game_count_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, _, _, _ = self.fixture(root)
            payload = json.loads(protocol_path.read_text(encoding="utf-8"))
            payload["games_per_level"] = 399
            protocol_path.write_text(json.dumps(payload), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "even number"):
                run_match_protocol.load_protocol(protocol_path)

    def test_malformed_required_fields_are_rejected_as_invalid_protocol(self) -> None:
        cases = (
            (
                "floating schema version",
                lambda value: value.__setitem__("schema_version", 1.0),
                "schema",
            ),
            ("missing runner", lambda value: value.pop("runner"), "runner"),
            (
                "invalid platform",
                lambda value: value.__setitem__("platform", "Windows"),
                "platform",
            ),
            (
                "empty chess version",
                lambda value: value.__setitem__("python_chess_version", ""),
                "python_chess_version",
            ),
            (
                "overflowing base time",
                lambda value: value["time_control"].__setitem__(
                    "base_seconds", 10**400
                ),
                "finite number",
            ),
            (
                "escaping opening path",
                lambda value: value["openings"].__setitem__("path", "../suite.epd"),
                "repository-relative",
            ),
            (
                "Windows rooted runner path",
                lambda value: value["runner"].__setitem__(
                    "path", "\\tools\\estimate_elo.py"
                ),
                "repository-relative",
            ),
            (
                "invalid Stockfish hash",
                lambda value: value["stockfish"].__setitem__("asset_sha256", "bad"),
                "stockfish.asset_sha256",
            ),
            (
                "missing Proton hash",
                lambda value: value["proton"].pop("binary_sha256"),
                "Proton binary_sha256",
            ),
        )
        for label, mutate, expected_error in cases:
            with self.subTest(label=label), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                protocol_path, _, _, _ = self.fixture(root)
                payload = json.loads(protocol_path.read_text(encoding="utf-8"))
                mutate(payload)
                protocol_path.write_text(json.dumps(payload), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, expected_error):
                    run_match_protocol.load_protocol(protocol_path)

    def test_runner_hash_mismatch_stops_before_play(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, _ = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            (root / "tools" / "estimate_elo.py").write_text("changed\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "runner SHA-256 mismatch"):
                run_match_protocol.verified_inputs(protocol, root, proton, stockfish)

    def test_repository_manifest_and_suite_are_self_consistent(self) -> None:
        protocol_path = ROOT / "matches" / "stockfish18_3000_60+0.6.json"
        protocol = run_match_protocol.load_protocol(protocol_path)
        runner = ROOT / protocol["runner"]["path"]
        openings = ROOT / protocol["openings"]["path"]
        loaded = run_match_protocol.estimate_elo.load_openings(openings)
        self.assertEqual(run_match_protocol.sha256_file(runner), protocol["runner"]["sha256"])
        self.assertEqual(
            run_match_protocol.sha256_file(openings), protocol["openings"]["sha256"]
        )
        self.assertEqual(len(loaded), protocol["openings"]["positions"])
        self.assertEqual(
            len({(item.fen, tuple(item.moves)) for item in loaded}),
            len(loaded),
        )

    def test_clean_worktree_requirement_runs_before_a_match(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            protocol_path, proton, stockfish, _ = self.fixture(root)
            protocol = run_match_protocol.load_protocol(protocol_path)
            protocol["require_clean_git"] = True
            with self.assertRaisesRegex(ValueError, "Git status preflight failed"):
                run_match_protocol.verified_inputs(protocol, root, proton, stockfish)


if __name__ == "__main__":
    unittest.main()
