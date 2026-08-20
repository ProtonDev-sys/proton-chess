#!/usr/bin/env python3
"""Focused tests for deterministic paired search comparisons."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import queue
import sys
import tempfile
import time
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import compare_search  # noqa: E402


START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
AFTER_E4 = "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3 0 1"


def result(
    bestmove: str = "e2e4",
    nodes: int = 100,
    score_type: str = "cp",
    score_value: int = 12,
    elapsed: int = 10,
) -> dict[str, object]:
    return {
        "bestmove": bestmove,
        "ponder": None,
        "wall_time_ms": float(elapsed) + 0.5,
        "raw_info": "test",
        "depth": 4,
        "seldepth": 6,
        "nodes": nodes,
        "nps": nodes * 100,
        "hashfull": 0,
        "time": elapsed,
        "score": {"type": score_type, "value": score_value, "bound": "exact"},
        "pv": [bestmove],
    }


class FakeSearchEngine:
    def __init__(self, name: str, log: list[str], nodes: int = 100) -> None:
        self.name = name
        self.log = log
        self.nodes = nodes

    def search(
        self, fen: str, go_command: str, limit: dict[str, object]
    ) -> dict[str, object]:
        self.log.append(self.name)
        self.log.append(fen)
        self.log.append(go_command)
        self.log.append(str(limit["kind"]))
        return result(nodes=self.nodes)


class CompareSearchTests(unittest.TestCase):
    def test_cli_defaults_and_mutually_exclusive_limits(self) -> None:
        args = compare_search.parse_args(["candidate", "baseline", "--json", "out"])
        self.assertEqual(args.depth, 10)
        self.assertIsNone(args.nodes)
        self.assertEqual(args.trials, 2)

        args = compare_search.parse_args(
            ["candidate", "baseline", "--nodes", "5000", "--json", "out"]
        )
        self.assertIsNone(args.depth)
        self.assertEqual(args.nodes, 5000)
        with self.assertRaisesRegex(ValueError, r"2\^64-1"):
            compare_search.parse_args(
                [
                    "candidate",
                    "baseline",
                    "--nodes",
                    str(2**64),
                    "--json",
                    "out",
                ]
            )
        with (
            contextlib.redirect_stderr(io.StringIO()),
            self.assertRaises(SystemExit),
        ):
            compare_search.parse_args(
                [
                    "candidate",
                    "baseline",
                    "--depth",
                    "4",
                    "--nodes",
                    "5000",
                    "--json",
                    "out",
                ]
            )
        for trials in ("0", "1"):
            with self.assertRaisesRegex(ValueError, "trials must be at least two"):
                compare_search.parse_args(
                    [
                        "candidate",
                        "baseline",
                        "--trials",
                        trials,
                        "--json",
                        "out",
                    ]
                )
        for timeout in ("nan", "inf", "1e20"):
            with self.assertRaisesRegex(ValueError, "no more than 86400"):
                compare_search.parse_args(
                    [
                        "candidate",
                        "baseline",
                        "--timeout",
                        timeout,
                        "--json",
                        "out",
                    ]
                )
        with self.assertRaisesRegex(ValueError, "labels must be different"):
            compare_search.parse_args(
                [
                    "candidate",
                    "baseline",
                    "--candidate-label",
                    "same",
                    "--baseline-label",
                    "same",
                    "--json",
                    "out",
                ]
            )

    def test_position_loader_is_strict_unique_and_canonical(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "positions.epd"
            path.write_text(
                f"# fixture\n{START_FEN}\n{AFTER_E4} trailing epd text\n",
                encoding="utf-8",
            )
            positions = compare_search.load_positions(path)
            self.assertEqual([item["index"] for item in positions], [1, 2])
            self.assertEqual(positions[1]["fen"], AFTER_E4)
            self.assertEqual(positions[1]["line_number"], 3)
            with self.assertRaisesRegex(ValueError, "contains only 2"):
                compare_search.load_positions(path, 3)

            path.write_text(f"{START_FEN}\n{START_FEN}\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "duplicate FEN"):
                compare_search.load_positions(path)

            path.write_text("not a fen\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "six-field FEN"):
                compare_search.load_positions(path)

    def test_search_result_uses_last_completed_info_and_validates_pv(self) -> None:
        lines = [
            "info depth 1 seldepth 1 score cp 4 nodes 20 nps 2000 time 1 pv d2d4",
            "info depth 2 seldepth 3 score cp 12 nodes 80 nps 8000 hashfull 0 time 2 pv e2e4 e7e5",
            "bestmove e2e4 ponder e7e5",
        ]
        parsed = compare_search.parse_search_result(lines[-1], lines, 2.75)
        self.assertEqual(parsed["bestmove"], "e2e4")
        self.assertEqual(parsed["ponder"], "e7e5")
        self.assertEqual(parsed["depth"], 2)
        self.assertEqual(parsed["score"], {"type": "cp", "value": 12, "bound": "exact"})
        self.assertEqual(parsed["pv"], ["e2e4", "e7e5"])
        compare_search.validate_search_result(
            START_FEN, parsed, {"kind": "depth", "value": 2}
        )

        mate = compare_search.parse_search_result(
            "bestmove e2e4",
            [
                "info depth 2 seldepth 2 score mate 3 nodes 90 nps 9000 time 2 pv e2e4"
            ],
            2.0,
        )
        self.assertEqual(mate["score"]["type"], "mate")
        with self.assertRaisesRegex(compare_search.EngineFailure, "requested 3"):
            compare_search.validate_search_result(
                START_FEN, parsed, {"kind": "depth", "value": 3}
            )
        parsed["pv"] = ["d2d4"]
        with self.assertRaisesRegex(compare_search.EngineFailure, "begin with bestmove"):
            compare_search.validate_search_result(
                START_FEN, parsed, {"kind": "nodes", "value": 100}
            )
        parsed["pv"] = ["e2e4", "e7e5"]
        with self.assertRaisesRegex(compare_search.EngineFailure, "above the 79 cap"):
            compare_search.validate_search_result(
                START_FEN, parsed, {"kind": "nodes", "value": 79}
            )

    def test_malformed_bestmove_bounds_and_metrics_are_rejected(self) -> None:
        valid_info = (
            "info depth 2 seldepth 2 score cp 1 nodes 10 nps 1000 "
            "hashfull 0 time 1 pv e2e4"
        )
        for bestmove in (
            "bestmove e2e4 junk",
            "bestmove e2e4 ponder nope",
            "bestmove e2e4 ponder e7e5 trailing",
        ):
            with self.subTest(bestmove=bestmove):
                with self.assertRaisesRegex(compare_search.EngineFailure, "bestmove"):
                    compare_search.parse_search_result(
                        bestmove, [valid_info, bestmove], 1.0
                    )
        with self.assertRaisesRegex(compare_search.EngineFailure, "contradictory"):
            compare_search.parse_search_result(
                "bestmove e2e4",
                [
                    "info depth 2 seldepth 2 score cp 1 lowerbound upperbound "
                    "nodes 10 time 1 pv e2e4"
                ],
                1.0,
            )
        with self.assertRaisesRegex(compare_search.EngineFailure, "negative"):
            compare_search.parse_search_result(
                "bestmove e2e4",
                [
                    "info depth 2 seldepth 2 score cp 1 nodes -1 time 1 pv e2e4"
                ],
                1.0,
            )

    def test_queue_deadline_does_not_block_on_silent_engine(self) -> None:
        engine = compare_search.EngineProcess.__new__(compare_search.EngineProcess)
        engine.label = "silent"
        engine.timeout = 0.02
        engine._lines = queue.Queue()
        engine._tail = []
        engine.process = mock.Mock()
        engine.process.poll.return_value = None
        started = time.monotonic()
        with self.assertRaisesRegex(TimeoutError, "silent timed out"):
            engine.read_until(lambda line: line == "readyok", "readyok")
        self.assertLess(time.monotonic() - started, 0.5)

    def test_position_command_is_round_tripped_before_search(self) -> None:
        engine = compare_search.EngineProcess.__new__(compare_search.EngineProcess)
        engine.label = "candidate"
        engine.send = mock.Mock()
        engine.read_until = mock.Mock(
            return_value=(f"info string fen {START_FEN}", [])
        )
        engine.set_position(START_FEN)
        self.assertEqual(
            engine.send.call_args_list,
            [mock.call(f"position fen {START_FEN}"), mock.call("d")],
        )

        engine.read_until.return_value = (f"info string fen {AFTER_E4}", [])
        with self.assertRaisesRegex(compare_search.EngineFailure, "different position"):
            engine.set_position(START_FEN)

    def test_configuration_is_full_strength_and_limiter_is_last(self) -> None:
        configuration = compare_search.engine_configuration(32)
        self.assertEqual(list(configuration), list(compare_search.OPTION_ORDER))
        self.assertEqual(configuration["Hash"], 32)
        self.assertEqual(configuration["Threads"], 1)
        self.assertFalse(configuration["UseBook"])
        self.assertFalse(configuration["HumanStyle"])
        self.assertEqual(configuration["HumanVariety"], 35)
        self.assertFalse(configuration["UCI_LimitStrength"])
        self.assertEqual(list(configuration)[-1], "UCI_LimitStrength")

    def test_pair_order_flips_by_position_and_trial(self) -> None:
        positions = [
            {"index": 1, "line_number": 1, "fen": START_FEN},
            {"index": 2, "line_number": 2, "fen": AFTER_E4},
        ]
        log: list[str] = []
        candidate = FakeSearchEngine("candidate", log, 110)
        baseline = FakeSearchEngine("baseline", log, 100)
        rows = compare_search.run_pairs(
            candidate,  # type: ignore[arg-type]
            baseline,  # type: ignore[arg-type]
            positions,
            "go depth 4",
            {"kind": "depth", "value": 4},
            1,
        )
        self.assertEqual(rows[0]["search_order"], ["candidate", "baseline"])
        self.assertEqual(rows[1]["search_order"], ["baseline", "candidate"])

        second = compare_search.run_pairs(
            candidate,  # type: ignore[arg-type]
            baseline,  # type: ignore[arg-type]
            positions,
            "go depth 4",
            {"kind": "depth", "value": 4},
            2,
        )
        self.assertEqual(second[0]["search_order"], ["baseline", "candidate"])
        self.assertEqual(second[1]["search_order"], ["candidate", "baseline"])

    def test_summary_separates_semantics_timing_and_node_cap_meaning(self) -> None:
        rows = [
            {
                "trial": trial,
                "position": 1,
                "candidate": result(nodes=110, elapsed=12),
                "baseline": result(nodes=100, elapsed=10),
            }
            for trial in (1, 2)
        ]
        depth = compare_search.summarize(rows, "depth")
        self.assertEqual(depth["bestmove_matches"], 2)
        self.assertEqual(depth["all_semantic_matches"], 0)
        self.assertEqual(depth["node_delta"], 20)
        self.assertAlmostEqual(depth["node_delta_percent"], 10.0)
        self.assertTrue(depth["deterministic_across_trials"])
        self.assertEqual(depth["candidate_repeatability"]["repeatable_positions"], 1)

        capped = compare_search.summarize(rows, "nodes")
        self.assertFalse(capped["node_efficiency_comparable"])
        self.assertNotIn("node_delta", capped)
        self.assertIn("last completed iteration", capped["info_nodes_note"])

        changed_ponder = copy.deepcopy(rows)
        changed_ponder[1]["candidate"]["ponder"] = "e7e5"
        repeatability = compare_search.summarize(changed_ponder, "depth")
        self.assertFalse(repeatability["deterministic_across_trials"])
        self.assertEqual(
            repeatability["candidate_repeatability"]["mismatches"],
            [{"position": 1, "fields": ["ponder"]}],
        )

    def test_cleanup_attempts_every_engine_after_an_error(self) -> None:
        calls: list[str] = []

        class ClosingEngine:
            def __init__(self, name: str, fail: bool = False) -> None:
                self.name = name
                self.fail = fail

            def close(self) -> None:
                calls.append(self.name)
                if self.fail:
                    raise RuntimeError(f"{self.name} close failed")

        errors = compare_search.close_engines(
            [ClosingEngine("candidate", True), ClosingEngine("baseline")]
        )
        self.assertEqual(calls, ["candidate", "baseline"])
        self.assertEqual(len(errors), 1)
        self.assertIn("candidate close failed", str(errors[0]))

        primary = RuntimeError("search failed")
        primary.add_note("engine cleanup also failed: kill timeout")
        payload = compare_search.error_payload(primary, {"phase": "search"})
        self.assertEqual(
            payload["notes"], ["engine cleanup also failed: kill timeout"]
        )

        with mock.patch("builtins.print", side_effect=BrokenPipeError):
            compare_search.print_json_report({"status": "complete"})

    def test_stage_detects_source_mutation_and_atomic_json_replaces(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            staged = root / "staged.bin"
            source.write_bytes(b"first")

            def mutating_copy(source_path: Path, destination_path: Path) -> None:
                destination_path.write_bytes(source_path.read_bytes())
                source_path.write_bytes(b"changed")

            with (
                mock.patch.object(
                    compare_search.shutil, "copy2", side_effect=mutating_copy
                ),
                self.assertRaisesRegex(ValueError, "changed while it was staged"),
            ):
                compare_search.stage_file(source, staged)
            self.assertFalse(staged.exists())

            report = root / "report.json"
            compare_search.write_json_atomic(report, {"status": "running"})
            compare_search.write_json_atomic(report, {"status": "complete"})
            self.assertEqual(json.loads(report.read_text(encoding="utf-8")),
                             {"status": "complete"})
            self.assertEqual(list(root.glob(".report.json.*.tmp")), [])

    def test_main_uses_fresh_processes_and_persists_complete_report(self) -> None:
        instances: list[object] = []

        class FakeProcess:
            def __init__(self, path: Path, label: str, timeout: float) -> None:
                self.label = label
                self.identity = {"name": "Proton Chess", "author": "test"}
                self.options = set(compare_search.OPTION_ORDER)
                self.closed = False
                self.configurations: list[dict[str, object]] = []
                instances.append(self)

            def initialize(self, configuration: dict[str, object]) -> None:
                self.configurations.append(dict(configuration))

            def search(
                self, fen: str, go_command: str, limit: dict[str, object]
            ) -> dict[str, object]:
                return result()

            def close(self) -> None:
                self.closed = True

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.bin"
            baseline = root / "baseline.bin"
            openings = root / "positions.epd"
            report_path = root / "report.json"
            candidate.write_bytes(b"candidate")
            baseline.write_bytes(b"baseline")
            openings.write_text(START_FEN + "\n", encoding="utf-8")
            with (
                mock.patch.object(compare_search, "EngineProcess", FakeProcess),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                exit_code = compare_search.main(
                    [
                        str(candidate),
                        str(baseline),
                        "--openings",
                        str(openings),
                        "--depth",
                        "4",
                        "--trials",
                        "2",
                        "--json",
                        str(report_path),
                    ]
                )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(exit_code, 0)
            self.assertEqual(report["status"], "complete")
            self.assertEqual(report["completed_search_pairs"], 2)
            self.assertEqual(report["rows"][0]["search_order"],
                             ["candidate", "baseline"])
            self.assertEqual(report["rows"][1]["search_order"],
                             ["baseline", "candidate"])
            self.assertEqual(len(instances), 4)
            self.assertTrue(all(instance.closed for instance in instances))

    def test_main_checkpoints_failure_context_and_closes_processes(self) -> None:
        instances: list[object] = []

        class FailingProcess:
            def __init__(self, path: Path, label: str, timeout: float) -> None:
                self.label = label
                self.identity = {"name": "Proton Chess", "author": "test"}
                self.options = set(compare_search.OPTION_ORDER)
                self.closed = False
                instances.append(self)

            def initialize(self, configuration: dict[str, object]) -> None:
                pass

            def search(
                self, fen: str, go_command: str, limit: dict[str, object]
            ) -> dict[str, object]:
                raise TimeoutError(f"{self.label} stalled")

            def close(self) -> None:
                self.closed = True

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.bin"
            baseline = root / "baseline.bin"
            openings = root / "positions.epd"
            report_path = root / "report.json"
            candidate.write_bytes(b"candidate")
            baseline.write_bytes(b"baseline")
            openings.write_text(START_FEN + "\n", encoding="utf-8")
            with (
                mock.patch.object(compare_search, "EngineProcess", FailingProcess),
                contextlib.redirect_stdout(io.StringIO()),
                contextlib.redirect_stderr(io.StringIO()),
            ):
                exit_code = compare_search.main(
                    [
                        str(candidate),
                        str(baseline),
                        "--openings",
                        str(openings),
                        "--depth",
                        "4",
                        "--json",
                        str(report_path),
                    ]
                )
            report = json.loads(report_path.read_text(encoding="utf-8"))
            self.assertEqual(exit_code, 1)
            self.assertEqual(report["status"], "failed")
            self.assertEqual(report["error"]["type"], "TimeoutError")
            self.assertEqual(report["error"]["context"]["phase"], "search")
            self.assertEqual(report["error"]["context"]["engine"], "candidate")
            self.assertTrue(all(instance.closed for instance in instances))


if __name__ == "__main__":
    unittest.main()
