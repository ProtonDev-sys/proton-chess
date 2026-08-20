#!/usr/bin/env python3
"""Focused tests for match clocks, adjudication, and game lifecycle."""

from __future__ import annotations

import asyncio
import contextlib
import io
import json
import sys
import tempfile
import threading
import unittest
from argparse import Namespace
from pathlib import Path
from unittest import mock

import chess
import chess.engine

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import estimate_elo  # noqa: E402


class ScriptedPlayer:
    def __init__(self, moves: list[str]) -> None:
        self.moves = iter(moves)
        self.calls: list[tuple[object, chess.engine.Limit]] = []

    def __call__(
        self,
        engine: object,
        board: chess.Board,
        limit: chess.engine.Limit,
        game_token: object,
        timeout_seconds: float,
    ) -> chess.engine.PlayResult:
        del engine, board, timeout_seconds
        self.calls.append((game_token, limit))
        return chess.engine.PlayResult(chess.Move.from_uci(next(self.moves)), None)


class EstimateEloTests(unittest.TestCase):
    STARTING_OPENING = estimate_elo.OpeningPosition(chess.STARTING_FEN, [])

    @staticmethod
    def record(
        pair: int,
        color: str,
        point: float,
        opening_index: int = 7,
        opening_fen: str = chess.STARTING_FEN,
    ) -> estimate_elo.GameRecord:
        return estimate_elo.GameRecord(
            pair=pair,
            proton_color=color,
            opening_index=opening_index,
            opening_fen=opening_fen,
            opening_moves=["e2e4"],
            point=point,
            termination="test",
            plies=1,
            moves=["e2e4"],
            white_clock_seconds=None,
            black_clock_seconds=None,
        )

    def test_game_preparation_binds_token_before_clocked_play(self) -> None:
        loop = asyncio.new_event_loop()
        loop_thread = threading.Thread(target=loop.run_forever)
        loop_thread.start()
        token = object()

        class PreparedProtocol:
            def __init__(self) -> None:
                self.loop = loop
                self.game: object | None = None
                self.new_games = 0
                self.pings = 0

            def _ucinewgame(self) -> None:
                self.new_games += 1

            async def ping(self) -> None:
                self.pings += 1

        class PreparedEngine:
            def __init__(self) -> None:
                self.protocol = PreparedProtocol()
                self.closed = False

            def close(self) -> None:
                self.closed = True

        engine = PreparedEngine()
        try:
            estimate_elo.prepare_engine_for_game(engine, token)
            self.assertIs(engine.protocol.game, token)
            self.assertEqual(engine.protocol.new_games, 1)
            self.assertEqual(engine.protocol.pings, 1)
            self.assertFalse(engine.closed)
        finally:
            loop.call_soon_threadsafe(loop.stop)
            loop_thread.join(timeout=1)
            loop.close()

    def test_async_watchdog_cancels_and_closes_stuck_engine(self) -> None:
        loop = asyncio.new_event_loop()
        loop_thread = threading.Thread(target=loop.run_forever)
        loop_thread.start()
        cancelled = threading.Event()

        class SlowProtocol:
            def __init__(self) -> None:
                self.loop = loop

            async def play(
                self,
                board: chess.Board,
                limit: chess.engine.Limit,
                *,
                game: object,
            ) -> chess.engine.PlayResult:
                del board, limit, game
                try:
                    await asyncio.sleep(10)
                finally:
                    cancelled.set()
                return chess.engine.PlayResult(None, None)

        class SlowEngine:
            def __init__(self) -> None:
                self.protocol = SlowProtocol()
                self.closed = False

            def close(self) -> None:
                self.closed = True

        engine = SlowEngine()
        try:
            with self.assertRaises(estimate_elo.EngineMoveTimeout):
                estimate_elo.play_with_watchdog(
                    engine, chess.Board(), chess.engine.Limit(time=1), object(), 0.01)
            self.assertTrue(engine.closed)
            self.assertTrue(cancelled.wait(timeout=1))
        finally:
            loop.call_soon_threadsafe(loop.stop)
            loop_thread.join(timeout=1)
            loop.close()

    def test_fischer_limits_update_clocks_and_share_game_token(self) -> None:
        player = ScriptedPlayer(["e2e4", "e7e5"])
        moments = iter((0.0, 0.25, 0.25, 0.65))
        token = object()
        outcome = estimate_elo.play_game(
            object(),
            object(),
            self.STARTING_OPENING,
            True,
            estimate_elo.TimeControl(None, 60.0, 0.6, 2.0),
            2,
            token,
            player=player,
            clock=lambda: next(moments),
        )

        self.assertEqual(outcome.point, 0.5)
        self.assertEqual(outcome.termination, "move limit")
        self.assertAlmostEqual(outcome.white_clock_seconds or 0.0, 60.35)
        self.assertAlmostEqual(outcome.black_clock_seconds or 0.0, 60.2)
        self.assertEqual([call[0] for call in player.calls], [token, token])
        first_limit = player.calls[0][1]
        second_limit = player.calls[1][1]
        self.assertEqual(first_limit.white_clock, 60.0)
        self.assertEqual(first_limit.black_clock, 60.0)
        self.assertEqual(first_limit.white_inc, 0.6)
        self.assertAlmostEqual(second_limit.white_clock or 0.0, 60.35)
        self.assertEqual(second_limit.black_clock, 60.0)

    def test_watchdog_timeout_is_a_loss_without_increment(self) -> None:
        def timeout_player(*args: object) -> chess.engine.PlayResult:
            del args
            raise estimate_elo.EngineMoveTimeout("test timeout")

        outcome = estimate_elo.play_game(
            object(),
            object(),
            self.STARTING_OPENING,
            True,
            estimate_elo.TimeControl(None, 60.0, 0.6, 2.0),
            2,
            object(),
            player=timeout_player,
            clock=lambda: 0.0,
        )
        self.assertEqual(outcome.point, 0.0)
        self.assertEqual(outcome.termination, "white timeout")
        self.assertEqual(outcome.white_clock_seconds, 0.0)
        self.assertEqual(outcome.moves, [])

    def test_increment_does_not_rescue_a_flagged_move(self) -> None:
        player = ScriptedPlayer(["e2e4"])
        moments = iter((0.0, 1.1))
        outcome = estimate_elo.play_game(
            object(),
            object(),
            self.STARTING_OPENING,
            True,
            estimate_elo.TimeControl(None, 1.0, 0.6, 2.0),
            1,
            object(),
            player=player,
            clock=lambda: next(moments),
        )
        self.assertEqual(outcome.point, 0.0)
        self.assertEqual(outcome.termination, "white flag")
        self.assertEqual(outcome.white_clock_seconds, 0.0)
        self.assertEqual(outcome.moves, [])

    def test_exact_clock_exhaustion_flags_before_increment(self) -> None:
        player = ScriptedPlayer(["e2e4"])
        moments = iter((0.0, 1.0))
        outcome = estimate_elo.play_game(
            object(),
            object(),
            self.STARTING_OPENING,
            True,
            estimate_elo.TimeControl(None, 1.0, 0.6, 2.0),
            1,
            object(),
            player=player,
            clock=lambda: next(moments),
        )
        self.assertEqual(outcome.point, 0.0)
        self.assertEqual(outcome.termination, "white flag")
        self.assertEqual(outcome.white_clock_seconds, 0.0)
        self.assertEqual(outcome.moves, [])

    def test_fixed_movetime_remains_supported(self) -> None:
        player = ScriptedPlayer(["e2e4"])
        moments = iter((0.0, 0.01))
        outcome = estimate_elo.play_game(
            object(),
            object(),
            self.STARTING_OPENING,
            True,
            estimate_elo.TimeControl(0.05, None, 0.0, 2.0),
            1,
            object(),
            player=player,
            clock=lambda: next(moments),
        )
        self.assertEqual(outcome.point, 0.5)
        self.assertIsNone(outcome.white_clock_seconds)
        self.assertEqual(player.calls[0][1].time, 0.05)

    def test_black_to_move_fen_starts_with_black(self) -> None:
        player = ScriptedPlayer(["e7e5", "e2e4"])
        moments = iter((0.0, 0.01, 0.01, 0.02))
        opening = estimate_elo.OpeningPosition(
            f"{chess.STARTING_BOARD_FEN} b KQkq - 0 1", []
        )
        outcome = estimate_elo.play_game(
            object(),
            object(),
            opening,
            True,
            estimate_elo.TimeControl(0.05, None, 0.0, 2.0),
            3,
            object(),
            player=player,
            clock=lambda: next(moments),
        )
        self.assertEqual(outcome.moves, ["e7e5", "e2e4"])
        self.assertEqual(outcome.plies, 3)
        self.assertEqual(outcome.termination, "move limit")

    def test_opening_schedule_is_deterministic_and_copied(self) -> None:
        openings = [
            estimate_elo.OpeningPosition(chess.STARTING_FEN, [move])
            for move in ("e2e4", "d2d4", "c2c4")
        ]
        first = estimate_elo.select_openings(openings, 5, 17)
        second = estimate_elo.select_openings(openings, 5, 17)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 5)
        first[0][1].moves.append("e7e5")
        self.assertEqual(len(openings[first[0][0]].moves), 1)

    def test_proton_seed_derivation_is_stable_and_domain_separated(self) -> None:
        base = estimate_elo.derive_proton_seed(17, 1200, 1320, 1, "white")
        self.assertEqual(
            base,
            estimate_elo.derive_proton_seed(17, 1200, 1320, 1, "white"),
        )
        variants = {
            base,
            estimate_elo.derive_proton_seed(18, 1200, 1320, 1, "white"),
            estimate_elo.derive_proton_seed(17, 1800, 1320, 1, "white"),
            estimate_elo.derive_proton_seed(17, 1200, 1800, 1, "white"),
            estimate_elo.derive_proton_seed(17, 1200, 1320, 2, "white"),
            estimate_elo.derive_proton_seed(17, 1200, 1320, 1, "black"),
        }
        self.assertEqual(len(variants), 6)
        self.assertTrue(all(0 < seed < 2**64 for seed in variants))

    def test_stockfish_range_resolution_records_requested_and_effective_elo(self) -> None:
        levels = estimate_elo.resolve_opponent_levels(
            [1200, 3000], estimate_elo.UciEloRange(1320, 3190, 1320)
        )
        self.assertEqual(
            levels,
            [
                estimate_elo.OpponentLevel(1200, 1320),
                estimate_elo.OpponentLevel(3000, 3000),
            ],
        )
        expected_score = 1.0 / (1.0 + 10.0 ** ((1320 - 1200) / 400.0))
        self.assertAlmostEqual(estimate_elo.logistic_elo(1320, expected_score), 1200.0)
        with self.assertRaisesRegex(ValueError, "advertised maximum"):
            estimate_elo.resolve_opponent_levels(
                [3200], estimate_elo.UciEloRange(1320, 3190, 1320)
            )
        with self.assertRaisesRegex(ValueError, "resolve to Elo 1320"):
            estimate_elo.resolve_opponent_levels(
                [1200, 1320], estimate_elo.UciEloRange(1320, 3190, 1320)
            )

    def test_engine_configuration_enables_limiters_last(self) -> None:
        class RecordingEngine:
            def __init__(self) -> None:
                self.configurations: list[dict[str, object]] = []

            def configure(self, options: dict[str, object]) -> None:
                self.configurations.append(options)

        proton = RecordingEngine()
        stockfish = RecordingEngine()
        estimate_elo.configure_engines(
            proton, stockfish, 1320, 64, proton_elo=1200, proton_seed=99
        )
        self.assertEqual(
            list(proton.configurations[0]),
            ["Hash", "UseBook", "HumanStyle", "UCI_Elo", "HumanSeed",
             "UCI_LimitStrength"],
        )
        self.assertEqual(proton.configurations[0]["HumanSeed"], "99")
        self.assertEqual(
            list(stockfish.configurations[0]),
            ["Hash", "Threads", "UCI_Elo", "UCI_LimitStrength"],
        )
        full_proton = RecordingEngine()
        full_stockfish = RecordingEngine()
        estimate_elo.configure_engines(full_proton, full_stockfish, 3000, 64)
        self.assertEqual(
            full_proton.configurations,
            [{"Hash": 64, "UseBook": False, "HumanStyle": False}],
        )
        with self.assertRaisesRegex(ValueError, "requires a deterministic seed"):
            estimate_elo.configure_engines(
                RecordingEngine(), RecordingEngine(), 1320, 64, proton_elo=1200
            )
        with self.assertRaisesRegex(ValueError, "requires --proton-elo"):
            estimate_elo.configure_engines(
                RecordingEngine(), RecordingEngine(), 1320, 64, proton_seed=99
            )
        self.assertEqual(
            estimate_elo.proton_options(64, None),
            {"Hash": 64, "UseBook": False, "HumanStyle": False},
        )
        self.assertEqual(
            estimate_elo.proton_options(64, 1200),
            {
                "Hash": 64,
                "UseBook": False,
                "HumanStyle": False,
                "UCI_Elo": 1200,
                "UCI_LimitStrength": True,
            },
        )

    def test_opening_loader_accepts_fen_and_legacy_move_lines(self) -> None:
        fen = "rnbqkbnr/ppp1pppp/8/3p4/8/2N5/PPPPPPPP/R1BQKBNR w KQkq - 0 2"
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "mixed.epd"
            path.write_text(
                f"# mixed test\n{fen}\nlegacy|e2e4 e7e5\nnot a position\n",
                encoding="utf-8",
            )
            openings = estimate_elo.load_openings(path)
        self.assertEqual(len(openings), 2)
        self.assertEqual(openings[0].fen, fen)
        self.assertEqual(openings[0].moves, [])
        self.assertEqual(openings[1].fen, chess.STARTING_FEN)
        self.assertEqual(openings[1].moves, ["e2e4", "e7e5"])

    def test_fixed_nodes_use_go_nodes_and_an_absolute_watchdog(self) -> None:
        control = estimate_elo.TimeControl(
            None, None, 0.0, 1.5, nodes_per_move=4096
        )
        limit = control.limit(None)
        self.assertEqual(limit.nodes, 4096)
        self.assertIsNone(limit.time)
        self.assertEqual(control.watchdog(chess.WHITE, None), 1.5)
        self.assertEqual(control.payload()["mode"], "fixed_nodes")
        self.assertEqual(control.payload()["nodes_per_move"], 4096)

    def test_build_time_control_accepts_fixed_nodes(self) -> None:
        args = Namespace(
            nodes=2048,
            move_time=None,
            base_seconds=None,
            increment=0.0,
            watchdog_grace=2.0,
        )
        control = estimate_elo.build_time_control(args)
        self.assertEqual(control.nodes_per_move, 2048)
        with self.assertRaisesRegex(ValueError, "nodes must be positive"):
            args.nodes = 0
            estimate_elo.build_time_control(args)

    def test_distinct_games_can_use_distinct_lifecycle_tokens(self) -> None:
        player = ScriptedPlayer(["e2e4", "e2e4"])
        control = estimate_elo.TimeControl(0.01, None, 0.0, 1.0)
        token_a = object()
        token_b = object()
        for token in (token_a, token_b):
            moments = iter((0.0, 0.001))
            estimate_elo.play_game(
                object(), object(), self.STARTING_OPENING, True, control, 1, token,
                player=player, clock=lambda moments=moments: next(moments))
        self.assertIs(player.calls[0][0], token_a)
        self.assertIs(player.calls[1][0], token_b)

    def test_drawn_pair_has_non_degenerate_conservative_interval(self) -> None:
        result = estimate_elo.summarize_level(3000, [
            self.record(1, "white", 0.5),
            self.record(1, "black", 0.5),
        ])
        self.assertEqual(result.pair_count, 1)
        self.assertEqual(result.pair_scores, [0.5])
        self.assertEqual(result.complete_pair_score, 0.5)
        self.assertEqual(result.pentanomial_counts["1.0"], 1)
        self.assertEqual((result.score_ci95_low, result.score_ci95_high), (0.0, 1.0))
        self.assertFalse(result.significant_above_50)

    def test_incomplete_pair_is_checkpointed_but_not_counted_as_sample(self) -> None:
        result = estimate_elo.summarize_level(3000, [self.record(1, "white", 1.0)])
        self.assertEqual(result.games, 1)
        self.assertEqual(result.pair_count, 0)
        self.assertIsNone(result.complete_pair_score)
        self.assertEqual(sum(result.pentanomial_counts.values()), 0)
        self.assertEqual((result.score_ci95_low, result.score_ci95_high), (0.0, 1.0))
        self.assertFalse(result.significant_above_50)

    def test_perfect_two_hundred_pair_result_clears_the_gate(self) -> None:
        records = [
            self.record(pair, color, 1.0)
            for pair in range(1, 201)
            for color in ("white", "black")
        ]
        result = estimate_elo.summarize_level(3000, records)
        self.assertEqual(result.games, 400)
        self.assertEqual(result.pair_count, 200)
        self.assertEqual(result.pentanomial_counts["2.0"], 200)
        self.assertGreater(result.score_ci95_low, 0.5)
        self.assertTrue(result.significant_above_50)

    def test_pentanomial_counts_cover_all_pair_outcomes(self) -> None:
        outcomes = ((0.0, 0.0), (0.0, 0.5), (0.5, 0.5), (0.5, 1.0), (1.0, 1.0))
        records = [
            self.record(pair, color, point)
            for pair, points in enumerate(outcomes, start=1)
            for color, point in zip(("white", "black"), points, strict=True)
        ]
        result = estimate_elo.summarize_level(3000, records)
        self.assertEqual(
            result.pentanomial_counts,
            {"0.0": 1, "0.5": 1, "1.0": 1, "1.5": 1, "2.0": 1},
        )

    def test_pair_validation_rejects_mismatched_openings(self) -> None:
        records = [
            self.record(1, "white", 0.5, opening_index=7),
            self.record(1, "black", 0.5, opening_index=8),
        ]
        with self.assertRaisesRegex(ValueError, "different openings"):
            estimate_elo.complete_pair_scores(records)

    def test_pair_validation_rejects_same_index_with_different_fens(self) -> None:
        records = [
            self.record(1, "white", 0.5, opening_fen=chess.STARTING_FEN),
            self.record(
                1,
                "black",
                0.5,
                opening_fen=f"{chess.STARTING_BOARD_FEN} b KQkq - 0 1",
            ),
        ]
        with self.assertRaisesRegex(ValueError, "different openings"):
            estimate_elo.complete_pair_scores(records)

    def test_summary_rejects_invalid_game_points(self) -> None:
        with self.assertRaisesRegex(ValueError, "game points"):
            estimate_elo.summarize_level(3000, [self.record(1, "white", 0.25)])

    def test_run_level_checkpoints_after_each_completed_game(self) -> None:
        checkpoints: list[estimate_elo.MatchResult] = []
        outcomes = [
            estimate_elo.GameOutcome(1.0, "test", 1, ["e2e4"], None, None),
            estimate_elo.GameOutcome(0.5, "test", 1, ["e2e4"], None, None),
        ]
        args = Namespace(games=2, seed=17, hash=16, max_plies=40)
        with (
            mock.patch.object(estimate_elo.chess.engine.SimpleEngine, "popen_uci", return_value=object()),
            mock.patch.object(estimate_elo, "configure_engines"),
            mock.patch.object(estimate_elo, "prepare_engine_for_game"),
            mock.patch.object(estimate_elo, "safe_quit"),
            mock.patch.object(estimate_elo, "play_game", side_effect=outcomes),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = estimate_elo.run_level(
                Path("proton"),
                Path("stockfish"),
                3000,
                [estimate_elo.OpeningPosition(chess.STARTING_FEN, ["e2e4"])],
                args,
                estimate_elo.TimeControl(0.01, None, 0.0, 1.0),
                checkpoint=checkpoints.append,
            )
        self.assertEqual([item.games for item in checkpoints], [1, 2])
        self.assertEqual([item.pair_count for item in checkpoints], [0, 1])
        self.assertIsNone(checkpoints[0].complete_pair_score)
        self.assertEqual(checkpoints[1].complete_pair_score, 0.75)
        self.assertEqual(result, checkpoints[-1])

    def test_run_level_records_exact_per_game_proton_seeds(self) -> None:
        outcomes = [
            estimate_elo.GameOutcome(0.5, "test", 1, ["e2e4"], None, None),
            estimate_elo.GameOutcome(0.5, "test", 1, ["e2e4"], None, None),
        ]
        args = Namespace(
            games=2,
            seed=17,
            hash=16,
            max_plies=40,
            proton_elo=1200,
        )
        configure = mock.Mock()
        with (
            mock.patch.object(
                estimate_elo.chess.engine.SimpleEngine,
                "popen_uci",
                return_value=object(),
            ),
            mock.patch.object(estimate_elo, "configure_engines", configure),
            mock.patch.object(estimate_elo, "prepare_engine_for_game"),
            mock.patch.object(estimate_elo, "safe_quit"),
            mock.patch.object(estimate_elo, "play_game", side_effect=outcomes),
            contextlib.redirect_stdout(io.StringIO()),
        ):
            result = estimate_elo.run_level(
                Path("proton"),
                Path("stockfish"),
                1320,
                [estimate_elo.OpeningPosition(chess.STARTING_FEN, ["e2e4"])],
                args,
                estimate_elo.TimeControl(0.01, None, 0.0, 1.0),
                requested_opponent_elo=1200,
            )
        expected = [
            estimate_elo.derive_proton_seed(17, 1200, 1320, 1, color)
            for color in ("white", "black")
        ]
        self.assertEqual(
            [record.proton_seed for record in result.records],
            [str(seed) for seed in expected],
        )
        self.assertEqual(result.opponent_elo, 1320)
        self.assertEqual(result.opponent_elo_requested, 1200)
        self.assertEqual(
            [call.args[-1] for call in configure.call_args_list], expected
        )

    def test_main_checkpoints_running_then_complete_across_levels(self) -> None:
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
            proton = root / "proton.exe"
            stockfish = root / "stockfish.exe"
            openings = root / "openings.txt"
            report_path = root / "match.json"
            proton.write_bytes(b"proton")
            stockfish.write_bytes(b"stockfish")
            openings.write_text("e2e4 e7e5\n", encoding="utf-8")
            args = Namespace(
                proton=proton,
                stockfish=stockfish,
                opponent_elo=[2400, 3000],
                proton_elo=None,
                games=2,
                move_time=0.01,
                base_seconds=None,
                increment=0.0,
                watchdog_grace=1.0,
                max_plies=40,
                hash=16,
                seed=17,
                openings=openings,
                json_path=report_path,
            )

            def fake_level(
                proton_path: Path,
                stockfish_path: Path,
                rating: int,
                opening_lines: list[estimate_elo.OpeningPosition],
                parsed_args: Namespace,
                time_control: estimate_elo.TimeControl,
                checkpoint: object = None,
                *,
                requested_opponent_elo: int | None = None,
            ) -> estimate_elo.MatchResult:
                del proton_path, stockfish_path, opening_lines, parsed_args, time_control
                self.assertEqual(requested_opponent_elo, rating)
                first = estimate_elo.summarize_level(
                    rating, [self.record(1, "white", 0.5)])
                complete = estimate_elo.summarize_level(
                    rating,
                    [self.record(1, "white", 0.5), self.record(1, "black", 0.5)],
                )
                assert callable(checkpoint)
                checkpoint(first)
                checkpoint(complete)
                return complete

            artifact = estimate_elo.EngineArtifact("test", "test", "test", "0" * 64)
            with (
                mock.patch.object(estimate_elo, "parse_args", return_value=args),
                mock.patch.object(estimate_elo, "identify_engine", return_value=artifact),
                mock.patch.object(
                    estimate_elo,
                    "inspect_uci_elo_range",
                    return_value=estimate_elo.UciEloRange(1320, 3190, 1320),
                ),
                mock.patch.object(estimate_elo, "run_level", side_effect=fake_level),
                mock.patch.object(estimate_elo, "write_json_atomic", side_effect=capture),
                mock.patch("builtins.print", side_effect=lambda *args, **kwargs: events.append("print")),
            ):
                self.assertEqual(estimate_elo.main(), 0)

            final = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(snapshots[0]["status"], "running")
        self.assertTrue(all(item["status"] == "running" for item in snapshots[:-1]))
        self.assertEqual(snapshots[-1]["status"], "complete")
        self.assertIsNotNone(snapshots[-1]["completed_utc"])
        self.assertEqual(events[-2:], ["write:complete", "print"])
        self.assertEqual(len(final["results"]), 2)
        self.assertEqual(final["status"], "complete")

    def test_main_distinguishes_requested_and_effective_calibration_elo(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            proton = root / "proton.exe"
            stockfish = root / "stockfish.exe"
            openings = root / "openings.txt"
            report_path = root / "match.json"
            proton.write_bytes(b"proton")
            stockfish.write_bytes(b"stockfish")
            openings.write_text("e2e4 e7e5\n", encoding="utf-8")
            args = Namespace(
                proton=proton,
                stockfish=stockfish,
                opponent_elo=None,
                proton_elo=1200,
                games=2,
                move_time=0.01,
                base_seconds=None,
                increment=0.0,
                watchdog_grace=1.0,
                max_plies=40,
                hash=16,
                seed=17,
                openings=openings,
                json_path=report_path,
            )

            def fake_level(
                proton_path: Path,
                stockfish_path: Path,
                rating: int,
                opening_lines: list[estimate_elo.OpeningPosition],
                parsed_args: Namespace,
                time_control: estimate_elo.TimeControl,
                checkpoint: object = None,
                *,
                requested_opponent_elo: int | None = None,
            ) -> estimate_elo.MatchResult:
                del proton_path, stockfish_path, opening_lines, time_control, checkpoint
                self.assertEqual(parsed_args.proton_elo, 1200)
                self.assertEqual((requested_opponent_elo, rating), (1200, 1320))
                return estimate_elo.summarize_level(
                    rating,
                    [self.record(1, "white", 0.5), self.record(1, "black", 0.5)],
                    opponent_elo_requested=requested_opponent_elo,
                )

            artifact = estimate_elo.EngineArtifact("test", "test", "test", "0" * 64)
            with (
                mock.patch.object(estimate_elo, "parse_args", return_value=args),
                mock.patch.object(estimate_elo, "identify_engine", return_value=artifact),
                mock.patch.object(
                    estimate_elo,
                    "inspect_uci_elo_range",
                    side_effect=[
                        estimate_elo.UciEloRange(1320, 3190, 1320),
                        estimate_elo.UciEloRange(800, 3000, 2800),
                    ],
                ),
                mock.patch.object(estimate_elo, "run_level", side_effect=fake_level),
                contextlib.redirect_stdout(io.StringIO()),
            ):
                self.assertEqual(estimate_elo.main(), 0)

            final = json.loads(report_path.read_text(encoding="utf-8"))
        self.assertEqual(final["proton_target_elo"], 1200)
        self.assertEqual(final["proton_seed_derivation"], estimate_elo.PROTON_SEED_DERIVATION)
        self.assertEqual(final["stockfish_requested_elo"], [1200])
        self.assertEqual(final["stockfish_effective_elo"], [1320])
        self.assertEqual(final["stockfish_options"]["UCI_Elo"], [1320])
        self.assertEqual(final["results"][0]["opponent_elo"], 1320)
        self.assertEqual(final["results"][0]["opponent_elo_requested"], 1200)
        self.assertEqual(
            final["proton_options"],
            {
                "Hash": 16,
                "UseBook": False,
                "HumanStyle": False,
                "UCI_Elo": 1200,
                "UCI_LimitStrength": True,
            },
        )

    def test_main_rejects_proton_elo_outside_the_advertised_range(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            proton = root / "proton.exe"
            stockfish = root / "stockfish.exe"
            openings = root / "openings.txt"
            proton.write_bytes(b"proton")
            stockfish.write_bytes(b"stockfish")
            openings.write_text("e2e4 e7e5\n", encoding="utf-8")

            for target in (799, 3001):
                with self.subTest(target=target):
                    args = Namespace(
                        proton=proton,
                        stockfish=stockfish,
                        opponent_elo=[3000],
                        proton_elo=target,
                        games=2,
                        move_time=0.01,
                        base_seconds=None,
                        increment=0.0,
                        watchdog_grace=1.0,
                        max_plies=40,
                        hash=16,
                        seed=17,
                        openings=openings,
                        json_path=None,
                    )
                    run_level = mock.Mock()
                    with (
                        mock.patch.object(estimate_elo, "parse_args", return_value=args),
                        mock.patch.object(
                            estimate_elo,
                            "inspect_uci_elo_range",
                            side_effect=[
                                estimate_elo.UciEloRange(1320, 3190, 1320),
                                estimate_elo.UciEloRange(800, 3000, 2800),
                            ],
                        ),
                        mock.patch.object(estimate_elo, "run_level", run_level),
                    ):
                        with self.assertRaisesRegex(
                            ValueError, "outside its advertised range"
                        ):
                            estimate_elo.main()
                    run_level.assert_not_called()

    def test_json_checkpoint_replaces_atomically(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "nested" / "match.json"
            estimate_elo.write_json_atomic(path, {"status": "running", "games": 1})
            self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["games"], 1)
            estimate_elo.write_json_atomic(path, {"status": "complete", "games": 2})
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(payload, {"status": "complete", "games": 2})
            self.assertEqual(list(path.parent.glob(f".{path.name}.*.tmp")), [])

    def test_failed_json_checkpoint_preserves_target_and_removes_temporary(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "match.json"
            estimate_elo.write_json_atomic(path, {"status": "running"})
            with self.assertRaises(TypeError):
                estimate_elo.write_json_atomic(path, {"invalid": object()})
            payload = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(payload, {"status": "running"})
            self.assertEqual(list(path.parent.glob(f".{path.name}.*.tmp")), [])


if __name__ == "__main__":
    unittest.main()
