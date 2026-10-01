#!/usr/bin/env python3
"""Focused tests for symmetric candidate-versus-baseline matches."""

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

from tools import compare_engines  # noqa: E402
from tools import estimate_elo  # noqa: E402


class RecordingEngine:
    def __init__(
        self,
        source: str = "test",
        supported: tuple[str, ...] = compare_engines.CONFIGURATION_ORDER,
    ) -> None:
        self.source = source
        self.options = {name: object() for name in supported}
        self.configurations: list[dict[str, object]] = []

    def configure(self, options: dict[str, object]) -> None:
        self.configurations.append(dict(options))


class CompareEnginesTests(unittest.TestCase):
    @staticmethod
    def artifact(label: str, digest: str) -> compare_engines.LabeledEngineArtifact:
        return compare_engines.LabeledEngineArtifact(
            label=label,
            name="Proton Chess",
            author="ProtonDev-sys",
            path=f"{label}.exe",
            sha256=digest,
        )

    @staticmethod
    def record(
        pair: int,
        color: str,
        point: float,
        opening_index: int = 7,
    ) -> compare_engines.AbGameRecord:
        return compare_engines.AbGameRecord(
            pair=pair,
            candidate_color=color,
            opening_index=opening_index,
            opening_fen=chess.STARTING_FEN,
            opening_moves=["e2e4"],
            point=point,
            termination="test",
            plies=1,
            moves=["e2e4"],
            white_clock_seconds=None,
            black_clock_seconds=None,
            engine_seed="17",
        )

    def test_full_strength_configuration_is_identical_and_limiter_is_last(self) -> None:
        candidate = RecordingEngine()
        baseline = RecordingEngine()
        configuration = compare_engines.configure_pair(
            candidate,
            baseline,
            compare_engines.CONFIGURATION_ORDER,
            16,
            123456789,
        )
        self.assertEqual(candidate.configurations, [configuration])
        self.assertEqual(baseline.configurations, [configuration])
        self.assertEqual(
            list(configuration), list(compare_engines.CONFIGURATION_ORDER)
        )
        self.assertEqual(configuration["Hash"], 16)
        self.assertEqual(configuration["Threads"], 1)
        self.assertFalse(configuration["UseBook"])
        self.assertFalse(configuration["HumanStyle"])
        self.assertEqual(configuration["HumanSkill"], 20)
        self.assertEqual(configuration["HumanVariety"], 35)
        self.assertEqual(configuration["HumanSeed"], "123456789")
        self.assertEqual(configuration["MoveOverhead"], 25)
        self.assertEqual(configuration["Contempt"], 0)
        self.assertEqual(list(configuration)[-1], "UCI_LimitStrength")
        self.assertFalse(configuration["UCI_LimitStrength"])
        self.assertNotIn("UCI_Elo", configuration)

    def test_profiles_reject_binary_or_option_asymmetry(self) -> None:
        candidate = compare_engines.EngineProfile(
            self.artifact("candidate", "1" * 64),
            compare_engines.CONFIGURATION_ORDER,
        )
        identical = compare_engines.EngineProfile(
            self.artifact("baseline", "1" * 64),
            compare_engines.CONFIGURATION_ORDER,
        )
        with self.assertRaisesRegex(ValueError, "different SHA-256"):
            compare_engines.validate_profiles(candidate, identical)

        missing_seed = compare_engines.EngineProfile(
            self.artifact("baseline", "2" * 64),
            tuple(name for name in compare_engines.CONFIGURATION_ORDER
                  if name != "HumanSeed"),
        )
        with self.assertRaisesRegex(ValueError, "same controlled UCI options"):
            compare_engines.validate_profiles(candidate, missing_seed)

        both_missing_seed = compare_engines.EngineProfile(
            self.artifact("candidate", "3" * 64),
            missing_seed.supported_options,
        )
        with self.assertRaisesRegex(ValueError, "missing HumanSeed"):
            compare_engines.validate_profiles(both_missing_seed, missing_seed)

    def test_parser_accepts_fixed_node_match_control(self) -> None:
        with mock.patch.object(
            sys,
            "argv",
            [
                "compare_engines.py",
                "candidate",
                "baseline",
                "--nodes",
                "4096",
                "--json",
                "report.json",
            ],
        ):
            args = compare_engines.parse_args()
        self.assertEqual(args.nodes, 4096)
        self.assertIsNone(args.move_time)
        self.assertIsNone(args.base_seconds)
        control = compare_engines.build_match_time_control(args)
        self.assertEqual(control.limit(None).nodes, 4096)
        self.assertEqual(control.watchdog(chess.WHITE, None), 2.0)
        self.assertEqual(control.payload()["mode"], "fixed_nodes")
        self.assertEqual(control.payload()["nodes_per_move"], 4096)

        args.nodes = 0
        with self.assertRaisesRegex(ValueError, "nodes must be positive"):
            compare_engines.build_match_time_control(args)

    def test_engine_seed_is_stable_and_domain_separated(self) -> None:
        base = compare_engines.derive_engine_seed(17, 1, "white")
        self.assertEqual(base, compare_engines.derive_engine_seed(17, 1, "white"))
        variants = {
            base,
            compare_engines.derive_engine_seed(18, 1, "white"),
            compare_engines.derive_engine_seed(17, 2, "white"),
            compare_engines.derive_engine_seed(17, 1, "black"),
        }
        self.assertEqual(len(variants), 4)
        self.assertTrue(all(0 < seed < 2**64 for seed in variants))
        with self.assertRaisesRegex(ValueError, "candidate color"):
            compare_engines.derive_engine_seed(17, 1, "green")

    def test_exact_sign_flip_probabilities_and_relative_summary(self) -> None:
        self.assertEqual(
            compare_engines.exact_sign_flip_probabilities([0.5, 0.5]),
            (1.0, 1.0),
        )
        p_gain, p_regression = compare_engines.exact_sign_flip_probabilities(
            [1.0] * 5
        )
        self.assertEqual(p_gain, 1.0 / 32.0)
        self.assertEqual(p_regression, 1.0)

        records = [
            self.record(pair, color, 1.0)
            for pair in range(1, 6)
            for color in ("white", "black")
        ]
        result = compare_engines.summarize(records, 0.05, final=True)
        payload = asdict(result)
        self.assertEqual(result.verdict, "gain")
        self.assertEqual(result.pair_scores, [1.0] * 5)
        self.assertEqual(result.pentanomial_counts["2.0"], 5)
        self.assertGreater(result.estimated_elo_delta, 0.0)
        self.assertNotIn("opponent_elo", payload)
        self.assertNotIn("estimated_elo", payload)

        perfect_records = [
            self.record(pair, color, 1.0)
            for pair in range(1, 201)
            for color in ("white", "black")
        ]
        running = compare_engines.summarize(perfect_records, 0.05)
        final = compare_engines.summarize(perfect_records, 0.05, final=True)
        self.assertGreater(running.score_ci95_low, 0.5)
        self.assertFalse(running.significant_above_50)
        self.assertTrue(final.significant_above_50)

    def test_run_match_rounds_to_pairs_and_applies_same_seed_to_both(self) -> None:
        args = Namespace(
            games=3,
            seed=17,
            hash=16,
            max_plies=40,
            alpha=0.05,
        )
        openings = [
            estimate_elo.OpeningPosition(chess.STARTING_FEN, [move])
            for move in ("e2e4", "d2d4", "c2c4")
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

        checkpoints: list[compare_engines.AbResult] = []
        original_select = estimate_elo.select_openings
        with (
            mock.patch.object(
                compare_engines.chess.engine.SimpleEngine,
                "popen_uci",
                side_effect=open_engine,
            ),
            mock.patch.object(compare_engines.estimate_elo, "prepare_engine_for_game"),
            mock.patch.object(compare_engines.estimate_elo, "safe_quit") as safe_quit,
            mock.patch.object(
                compare_engines.estimate_elo,
                "play_game",
                side_effect=outcomes,
            ) as play_game,
            mock.patch.object(
                compare_engines.estimate_elo,
                "select_openings",
                wraps=original_select,
            ) as select_openings,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = compare_engines.run_match(
                Path("candidate.exe"),
                Path("baseline.exe"),
                compare_engines.CONFIGURATION_ORDER,
                openings,
                args,
                estimate_elo.TimeControl(0.01, None, 0.0, 1.0),
                checkpoint=checkpoints.append,
            )

        self.assertEqual(result.games, 4)
        self.assertEqual([item.games for item in checkpoints], [1, 2, 3, 4])
        self.assertTrue(all(item.verdict == "running" for item in checkpoints))
        self.assertEqual(result.verdict, "inconclusive")
        self.assertEqual(len(engines), 8)
        self.assertEqual(safe_quit.call_count, 8)
        select_openings.assert_called_once_with(openings, 2, 17)
        self.assertEqual(
            [record.candidate_color for record in result.records],
            ["white", "black", "white", "black"],
        )
        for offset in range(0, len(engines), 2):
            self.assertEqual(engines[offset].source, "candidate.exe")
            self.assertEqual(engines[offset + 1].source, "baseline.exe")
            candidate_configuration = engines[offset].configurations[0]
            baseline_configuration = engines[offset + 1].configurations[0]
            self.assertEqual(candidate_configuration, baseline_configuration)
            self.assertEqual(
                candidate_configuration["HumanSeed"],
                result.records[offset // 2].engine_seed,
            )
        expected_seeds = [
            str(compare_engines.derive_engine_seed(17, pair, color))
            for pair in (1, 2)
            for color in ("white", "black")
        ]
        self.assertEqual(
            [record.engine_seed for record in result.records], expected_seeds
        )
        for index, call in enumerate(play_game.call_args_list):
            self.assertEqual(call.args[0].source, "candidate.exe")
            self.assertEqual(call.args[1].source, "baseline.exe")
            self.assertEqual(call.args[3], index % 2 == 0)
        for first, second in zip(result.records[::2], result.records[1::2], strict=True):
            self.assertEqual(
                (first.opening_index, first.opening_fen, first.opening_moves),
                (second.opening_index, second.opening_fen, second.opening_moves),
            )

    def test_reported_seed_is_marked_per_game(self) -> None:
        options = compare_engines.reported_options(
            compare_engines.CONFIGURATION_ORDER, 32
        )
        self.assertEqual(options["Hash"], 32)
        self.assertEqual(
            options["HumanSeed"],
            "per-game; see result.records[].engine_seed",
        )

    def test_staged_copy_is_immutable_after_source_changes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / "source.bin"
            staged = root / "staged.bin"
            source.write_bytes(b"first")
            digest = compare_engines.stage_file(source, staged)
            source.write_bytes(b"second")
            self.assertEqual(staged.read_bytes(), b"first")
            self.assertEqual(estimate_elo.sha256_file(staged), digest)
            self.assertNotEqual(estimate_elo.sha256_file(source), digest)

    def test_baseline_launch_failure_closes_started_candidate(self) -> None:
        args = Namespace(
            games=2,
            seed=17,
            hash=16,
            max_plies=40,
            alpha=0.05,
        )
        candidate = RecordingEngine("candidate.exe")
        with (
            mock.patch.object(
                compare_engines.chess.engine.SimpleEngine,
                "popen_uci",
                side_effect=[candidate, RuntimeError("baseline launch failed")],
            ),
            mock.patch.object(compare_engines.estimate_elo, "safe_quit") as safe_quit,
            contextlib.redirect_stdout(io.StringIO()),
        ):
            with self.assertRaisesRegex(RuntimeError, "baseline launch failed"):
                compare_engines.run_match(
                    Path("candidate.exe"),
                    Path("baseline.exe"),
                    compare_engines.CONFIGURATION_ORDER,
                    [estimate_elo.OpeningPosition(chess.STARTING_FEN, ["e2e4"])],
                    args,
                    estimate_elo.TimeControl(0.01, None, 0.0, 1.0),
                )
        safe_quit.assert_called_once_with(candidate)

    def test_main_writes_running_checkpoints_then_complete_report(self) -> None:
        snapshots: list[dict[str, object]] = []
        events: list[str] = []
        original_write = estimate_elo.write_json_atomic

        def capture(path: Path, payload: object) -> None:
            snapshot = json.loads(json.dumps(payload))
            snapshots.append(snapshot)
            events.append(f"write:{snapshot['status']}")
            original_write(path, payload)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate_path = root / "candidate.exe"
            baseline_path = root / "baseline.exe"
            openings_path = root / "openings.epd"
            report_path = root / "report.json"
            candidate_path.write_bytes(b"candidate")
            baseline_path.write_bytes(b"baseline")
            openings_path.write_text(chess.STARTING_FEN + "\n", encoding="utf-8")
            openings_hash = estimate_elo.sha256_file(openings_path)
            args = Namespace(
                candidate=candidate_path,
                baseline=baseline_path,
                candidate_label="new",
                baseline_label="old",
                games=3,
                move_time=0.01,
                base_seconds=None,
                increment=0.0,
                watchdog_grace=1.0,
                max_plies=40,
                hash=16,
                seed=17,
                alpha=0.05,
                openings=openings_path,
                json_path=report_path,
            )
            candidate_profile = compare_engines.EngineProfile(
                self.artifact("new", estimate_elo.sha256_file(candidate_path)),
                compare_engines.CONFIGURATION_ORDER,
            )
            baseline_profile = compare_engines.EngineProfile(
                self.artifact("old", estimate_elo.sha256_file(baseline_path)),
                compare_engines.CONFIGURATION_ORDER,
            )
            candidate_profile = compare_engines.EngineProfile(
                compare_engines.LabeledEngineArtifact(
                    **{**asdict(candidate_profile.artifact), "path": str(candidate_path.resolve())}
                ),
                candidate_profile.supported_options,
            )
            baseline_profile = compare_engines.EngineProfile(
                compare_engines.LabeledEngineArtifact(
                    **{**asdict(baseline_profile.artifact), "path": str(baseline_path.resolve())}
                ),
                baseline_profile.supported_options,
            )
            partial = compare_engines.summarize(
                [self.record(1, "white", 0.5)], 0.05
            )
            complete = compare_engines.summarize(
                [
                    self.record(1, "white", 0.5),
                    self.record(1, "black", 0.5),
                ],
                0.05,
                final=True,
            )

            def fake_match(*args: object, checkpoint: object = None, **kwargs: object) -> compare_engines.AbResult:
                del args, kwargs
                assert callable(checkpoint)
                checkpoint(partial)
                return complete

            with (
                mock.patch.object(compare_engines, "parse_args", return_value=args),
                mock.patch.object(
                    compare_engines,
                    "inspect_engine",
                    side_effect=[candidate_profile, baseline_profile],
                ) as inspect_engine,
                mock.patch.object(
                    compare_engines, "run_match", side_effect=fake_match
                ) as run_match,
                mock.patch.object(
                    compare_engines.estimate_elo,
                    "write_json_atomic",
                    side_effect=capture,
                ),
                mock.patch("builtins.print", side_effect=lambda *a, **k: events.append("print")),
            ):
                self.assertEqual(compare_engines.main(), 0)

            final = json.loads(report_path.read_text(encoding="utf-8"))

            candidate_hash = estimate_elo.sha256_file(candidate_path)
            baseline_hash = estimate_elo.sha256_file(baseline_path)
            inspect_calls = inspect_engine.call_args_list
            self.assertEqual(len(inspect_calls), 2)
            staged_candidate = inspect_calls[0].args[0]
            staged_baseline = inspect_calls[1].args[0]
            self.assertNotEqual(staged_candidate, candidate_path.resolve())
            self.assertNotEqual(staged_baseline, baseline_path.resolve())
            self.assertEqual(staged_candidate.name, "candidate.exe")
            self.assertEqual(staged_baseline.name, "baseline.exe")
            self.assertEqual(staged_candidate.parent, staged_baseline.parent)
            self.assertEqual(
                inspect_calls[0].args[1:],
                ("new", candidate_path.resolve(), candidate_hash),
            )
            self.assertEqual(
                inspect_calls[1].args[1:],
                ("old", baseline_path.resolve(), baseline_hash),
            )
            self.assertEqual(run_match.call_args.args[0], staged_candidate)
            self.assertEqual(run_match.call_args.args[1], staged_baseline)

        self.assertEqual(snapshots[0]["status"], "running")
        self.assertIsNone(snapshots[0]["result"])
        self.assertEqual(snapshots[1]["status"], "running")
        self.assertEqual(snapshots[1]["result"]["verdict"], "running")
        self.assertTrue(all(item["completed_utc"] is None for item in snapshots[:-1]))
        self.assertEqual(snapshots[-1]["status"], "complete")
        self.assertEqual(events[-2:], ["write:complete", "print"])
        self.assertEqual(final["requested_games"], 3)
        self.assertEqual(final["scheduled_games"], 4)
        self.assertEqual(final["exact_alpha"], 0.05)
        self.assertEqual(final["candidate"]["label"], "new")
        self.assertEqual(final["baseline"]["label"], "old")
        self.assertNotEqual(final["candidate"]["sha256"], final["baseline"]["sha256"])
        self.assertEqual(final["candidate_options"], final["baseline_options"])
        self.assertIn("compare_engines.py", final["tool"]["path"])
        self.assertIn("estimate_elo.py", final["match_core"]["path"])
        self.assertEqual(
            final["openings"]["sha256"], openings_hash
        )
        self.assertEqual(
            final["tool"]["sha256"],
            estimate_elo.sha256_file(Path(compare_engines.__file__).resolve()),
        )
        self.assertEqual(
            final["match_core"]["sha256"],
            estimate_elo.sha256_file(Path(estimate_elo.__file__).resolve()),
        )
        self.assertEqual(final["result"], asdict(complete))
        self.assertEqual(final["result"]["games"], 2)
        self.assertEqual(final["result"]["pair_count"], 1)
        self.assertEqual(final["result"]["verdict"], "inconclusive")
        self.assertIsNotNone(final["completed_utc"])
        self.assertNotIn("opponent_elo", final)

    def test_main_rejects_invalid_inputs_before_engine_inspection(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            candidate = root / "candidate.exe"
            baseline = root / "baseline.exe"
            openings = root / "openings.epd"
            candidate.write_bytes(b"candidate")
            baseline.write_bytes(b"baseline")
            openings.write_text(chess.STARTING_FEN + "\n", encoding="utf-8")
            base = dict(
                candidate=candidate,
                baseline=baseline,
                candidate_label="candidate",
                baseline_label="baseline",
                games=2,
                move_time=0.01,
                base_seconds=None,
                increment=0.0,
                watchdog_grace=1.0,
                max_plies=40,
                hash=16,
                seed=17,
                alpha=0.05,
                openings=openings,
                json_path=root / "report.json",
            )
            cases = [
                ({"baseline": candidate}, "paths must be different"),
                ({"baseline_label": "candidate"}, "labels must be"),
                ({"candidate_label": "  "}, "labels must be"),
                ({"alpha": 0.0}, "alpha must be"),
            ]
            for changes, message in cases:
                with self.subTest(changes=changes):
                    args = Namespace(**{**base, **changes})
                    with (
                        mock.patch.object(compare_engines, "parse_args", return_value=args),
                        mock.patch.object(compare_engines, "inspect_engine") as inspect,
                    ):
                        with self.assertRaisesRegex(ValueError, message):
                            compare_engines.main()
                    inspect.assert_not_called()


if __name__ == "__main__":
    unittest.main()
