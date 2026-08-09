#!/usr/bin/env python3
"""Focused tests for match clocks, adjudication, and game lifecycle."""

from __future__ import annotations

import asyncio
import sys
import threading
import unittest
from pathlib import Path

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
            [],
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
            [],
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
            [],
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
            [],
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
            [],
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

    def test_opening_schedule_is_deterministic_and_copied(self) -> None:
        openings = [["e2e4"], ["d2d4"], ["c2c4"]]
        first = estimate_elo.select_openings(openings, 5, 17)
        second = estimate_elo.select_openings(openings, 5, 17)
        self.assertEqual(first, second)
        self.assertEqual(len(first), 5)
        first[0][1].append("e7e5")
        self.assertEqual(openings[first[0][0]], [first[0][1][0]])

    def test_distinct_games_can_use_distinct_lifecycle_tokens(self) -> None:
        player = ScriptedPlayer(["e2e4", "e2e4"])
        control = estimate_elo.TimeControl(0.01, None, 0.0, 1.0)
        token_a = object()
        token_b = object()
        for token in (token_a, token_b):
            moments = iter((0.0, 0.001))
            estimate_elo.play_game(
                object(), object(), [], True, control, 1, token,
                player=player, clock=lambda moments=moments: next(moments))
        self.assertIs(player.calls[0][0], token_a)
        self.assertIs(player.calls[1][0], token_b)


if __name__ == "__main__":
    unittest.main()
