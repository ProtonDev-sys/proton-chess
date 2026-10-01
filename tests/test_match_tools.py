import argparse
from pathlib import Path
import sys
import tempfile
import queue
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

from match_stats import score_bounds
from paired_match import GameResult, load_openings, play_game, summarize
from benchmark_uci import read_until


class MatchToolsTests(unittest.TestCase):
    def test_silent_engine_benchmark_times_out(self):
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            read_until(None, queue.Queue(), "readyok", timeout=0.02)
        self.assertLess(time.monotonic() - started, 1)

    def test_perfect_small_sample_is_not_certainty(self):
        lower, upper = score_bounds([1.0, 1.0])
        self.assertEqual(lower, 0)
        self.assertEqual(upper, 1)

    def test_bounds_shrink_with_more_pairs(self):
        small_lower, _ = score_bounds([1.0] * 20)
        large_lower, _ = score_bounds([1.0] * 200)
        self.assertGreater(large_lower, small_lower)
        self.assertLess(large_lower, 1)

    def test_pair_effective_sample_size(self):
        lower, upper = score_bounds([1.0, 0.0] * 100)
        self.assertAlmostEqual(lower, 0.3776126584659592)
        self.assertAlmostEqual(upper, 0.6223873415340408)

    def test_unresolved_games_are_not_draws(self):
        records = [GameResult(pair, color, None, "ply-limit", 40, "*")
                   for pair in range(100) for color in (True, False)]
        result = summarize(records, 2200)
        self.assertEqual(result["draws"], 0)
        self.assertEqual(result["unresolved"], 200)
        self.assertEqual(result["pessimistic_score"], 0)
        self.assertFalse(result["passes"])

    def test_reject_invalid_scores_and_incomplete_pairs(self):
        for points in ([], [1.0], [0.3, 0.5]):
            with self.assertRaises(ValueError):
                score_bounds(points)

    def test_duplicate_pair_is_rejected(self):
        record = GameResult(0, True, 1.0, "checkmate", 40, "1-0")
        with self.assertRaises(ValueError):
            summarize([record, record], 2200)

    def test_incomplete_pair_is_rejected(self):
        records = [GameResult(pair, True, 1.0, "checkmate", 40, "1-0") for pair in range(2)]
        with self.assertRaises(ValueError):
            summarize(records, 2200)

    def test_stricter_confidence_widens_bound(self):
        lower95, _ = score_bounds([1.0] * 100)
        lower975, _ = score_bounds([1.0] * 100, 0.975)
        self.assertLess(lower975, lower95)

    def test_opening_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "openings.txt"
            path.write_text("e2e5\n", encoding="utf-8")
            with self.assertRaises(ValueError):
                load_openings(path)
            path.write_text("# comment\n5 | e2e4 e7e5\n", encoding="utf-8")
            self.assertEqual(load_openings(path), [["e2e4", "e7e5"]])

    def test_capped_game_stays_unresolved(self):
        args = argparse.Namespace(max_plies=2, move_time=0.1)
        engine = argparse.Namespace(id={"name": "test"})
        result = play_game(engine, engine, ["e2e4", "e7e5"], True, args, 0, 2200)
        self.assertIsNone(result.point)
        self.assertEqual(result.termination, "ply-limit")

    def test_terminal_game_at_cap_is_not_unresolved(self):
        args = argparse.Namespace(max_plies=4, move_time=0.1)
        engine = argparse.Namespace(id={"name": "test"})
        result = play_game(engine, engine, ["f2f3", "e7e5", "g2g4", "d8h4"],
                           True, args, 0, 2200)
        self.assertEqual(result.point, 0)
        self.assertEqual(result.termination, "checkmate")


if __name__ == "__main__":
    unittest.main()
