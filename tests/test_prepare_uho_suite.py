#!/usr/bin/env python3
"""Focused tests for deterministic UHO suite derivation."""

from __future__ import annotations

import tempfile
import sys
import unittest
from pathlib import Path

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import prepare_uho_suite


class PrepareUhoSuiteTests(unittest.TestCase):
    @staticmethod
    def sample_fens() -> list[str]:
        fens: list[str] = []
        for move in ("e2e4", "d2d4", "c2c4", "g1f3", "b1c3"):
            board = chess.Board()
            board.push_uci(move)
            fens.append(board.fen(en_passant="fen"))
        return fens

    def test_generation_is_deterministic_and_selects_unique_legal_positions(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.epd"
            first = root / "first.epd"
            second = root / "second.epd"
            source.write_text("\n".join(self.sample_fens()) + "\n", encoding="utf-8")
            source_hash = prepare_uho_suite.sha256_file(source)
            result = prepare_uho_suite.generate_suite(
                source,
                first,
                expected_source_sha256=source_hash,
                count=3,
                seed="test-seed",
            )
            prepare_uho_suite.generate_suite(
                source,
                second,
                expected_source_sha256=source_hash,
                count=3,
                seed="test-seed",
            )
            positions = [
                line for line in first.read_text(encoding="utf-8").splitlines()
                if line and not line.startswith("#")
            ]
            self.assertEqual(first.read_bytes(), second.read_bytes())
            self.assertEqual(result.source_positions, 5)
            self.assertEqual(result.candidate_pool_size, 5)
            self.assertEqual(result.rejected_candidates_before_selection, 0)
            self.assertEqual(len(positions), 3)
            self.assertEqual(len(set(positions)), 3)
            self.assertTrue(all(chess.Board(fen).is_valid() for fen in positions))

    def test_source_hash_mismatch_stops_generation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.epd"
            source.write_text(self.sample_fens()[0] + "\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "SHA-256 mismatch"):
                prepare_uho_suite.generate_suite(
                    source,
                    root / "output.epd",
                    expected_source_sha256="0" * 64,
                    count=1,
                )

    def test_invalid_and_terminal_positions_are_rejected(self) -> None:
        valid = self.sample_fens()[0]
        invalid = "not a fen"
        terminal = "7k/5Q2/7K/8/8/8/8/8 b - - 0 1"
        seed = next(
            candidate
            for candidate in (f"reject-test-{index}" for index in range(1000))
            if prepare_uho_suite.selection_rank(candidate, invalid)
            < prepare_uho_suite.selection_rank(candidate, valid)
            and prepare_uho_suite.selection_rank(candidate, terminal)
            < prepare_uho_suite.selection_rank(candidate, valid)
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.epd"
            source.write_text(f"{invalid}\n{terminal}\n{valid}\n", encoding="utf-8")
            source_hash = prepare_uho_suite.sha256_file(source)
            result = prepare_uho_suite.generate_suite(
                source,
                root / "output.epd",
                expected_source_sha256=source_hash,
                count=1,
                seed=seed,
            )
            self.assertEqual(result.source_positions, 3)
            self.assertEqual(result.candidate_pool_size, 3)
            self.assertEqual(result.rejected_candidates_before_selection, 2)


if __name__ == "__main__":
    unittest.main()
