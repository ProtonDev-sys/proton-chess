#!/usr/bin/env python3
"""Run reproducible paired matches against Stockfish UCI_Elo levels."""

from __future__ import annotations

import argparse
import asyncio
import concurrent.futures
import hashlib
import json
import math
import os
import platform
import random
import statistics
import subprocess
import sys
import time
from collections.abc import Callable
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import chess
import chess.engine


@dataclass(frozen=True)
class TimeControl:
    move_time: float | None
    base_seconds: float | None
    increment_seconds: float
    watchdog_grace_seconds: float

    def limit(self, clocks: dict[chess.Color, float] | None) -> chess.engine.Limit:
        if self.move_time is not None:
            return chess.engine.Limit(time=self.move_time)
        assert clocks is not None
        return chess.engine.Limit(
            white_clock=max(0.0, clocks[chess.WHITE]),
            black_clock=max(0.0, clocks[chess.BLACK]),
            white_inc=self.increment_seconds,
            black_inc=self.increment_seconds,
        )

    def watchdog(self, side: chess.Color, clocks: dict[chess.Color, float] | None) -> float:
        if self.move_time is not None:
            allowance = self.move_time
        else:
            assert clocks is not None
            allowance = clocks[side]
        return max(0.001, allowance + self.watchdog_grace_seconds)

    def payload(self) -> dict[str, float | str | None]:
        return {
            "mode": "fixed_movetime" if self.move_time is not None else "fischer",
            "move_time_seconds": self.move_time,
            "base_seconds": self.base_seconds,
            "increment_seconds": self.increment_seconds,
            "watchdog_grace_seconds": self.watchdog_grace_seconds,
        }


@dataclass
class EngineArtifact:
    name: str
    author: str
    path: str
    sha256: str


@dataclass
class InputArtifact:
    path: str
    sha256: str


@dataclass
class GameOutcome:
    point: float
    termination: str
    plies: int
    moves: list[str]
    white_clock_seconds: float | None
    black_clock_seconds: float | None


@dataclass
class GameRecord:
    pair: int
    proton_color: str
    opening_index: int
    opening_moves: list[str]
    point: float
    termination: str
    plies: int
    moves: list[str]
    white_clock_seconds: float | None
    black_clock_seconds: float | None


@dataclass
class MatchResult:
    opponent_elo: int
    games: int
    wins: int
    draws: int
    losses: int
    score: float
    estimated_elo: float
    records: list[GameRecord]


@dataclass
class MatchReport:
    schema_version: int
    created_utc: str
    seed: int
    requested_games_per_level: int
    max_plies: int
    hash_mb: int
    time_control: dict[str, float | str | None]
    proton: EngineArtifact
    stockfish: EngineArtifact
    openings: InputArtifact
    tool: InputArtifact
    git_revision: str | None
    git_dirty: bool | None
    python_version: str
    python_chess_version: str
    host: dict[str, Any]
    proton_options: dict[str, Any]
    stockfish_options: dict[str, Any]
    results: list[MatchResult]


class EngineMoveTimeout(RuntimeError):
    """Raised when an engine does not return before the external watchdog."""


PlayFunction = Callable[
    [Any, chess.Board, chess.engine.Limit, object, float],
    chess.engine.PlayResult,
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run paired Proton matches against limited-strength Stockfish."
    )
    parser.add_argument("proton", type=Path)
    parser.add_argument("stockfish", type=Path)
    parser.add_argument("--opponent-elo", type=int, action="append", required=True)
    parser.add_argument("--games", type=int, default=20,
                        help="games per opponent level; rounded up to an even number")
    timing = parser.add_mutually_exclusive_group()
    timing.add_argument("--move-time", type=float,
                        help="legacy fixed seconds per move (default: 0.05)")
    timing.add_argument("--base-seconds", type=float,
                        help="Fischer starting clock in seconds")
    parser.add_argument("--increment", type=float, default=0.0,
                        help="Fischer increment in seconds")
    parser.add_argument("--watchdog-grace", type=float, default=2.0,
                        help="extra wall-clock seconds before killing a stuck engine")
    parser.add_argument("--max-plies", type=int, default=240)
    parser.add_argument("--hash", type=int, default=64)
    parser.add_argument("--seed", type=int, default=20260805)
    parser.add_argument("--openings", type=Path,
                        default=Path("openings/book_lines.txt"))
    parser.add_argument("--json", type=Path, dest="json_path")
    return parser.parse_args()


def load_openings(path: Path) -> list[list[str]]:
    openings: list[list[str]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        moves = line.split("|", 1)[-1].split()
        board = chess.Board()
        try:
            for text in moves:
                board.push_uci(text)
        except ValueError:
            continue
        if not board.is_game_over(claim_draw=True):
            openings.append(moves)
    if not openings:
        raise ValueError(f"no valid openings found in {path}")
    return openings


def select_openings(
    openings: list[list[str]], pair_count: int, seed: int
) -> list[tuple[int, list[str]]]:
    rng = random.Random(seed)
    indexed = list(enumerate(openings))
    selected = rng.sample(indexed, k=min(pair_count, len(indexed)))
    while len(selected) < pair_count:
        selected.append(rng.choice(indexed))
    return [(index, list(moves)) for index, moves in selected]


def logistic_elo(opponent_elo: int, score: float) -> float:
    score = min(0.999, max(0.001, score))
    return opponent_elo + 400.0 * math.log10(score / (1.0 - score))


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_revision(root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    revision = result.stdout.strip()
    return revision if result.returncode == 0 and revision else None


def git_dirty(root: Path) -> bool | None:
    try:
        result = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    return bool(result.stdout.strip()) if result.returncode == 0 else None


def safe_quit(engine: chess.engine.SimpleEngine) -> None:
    try:
        engine.quit()
    except (chess.engine.EngineError, TimeoutError, concurrent.futures.CancelledError):
        engine.close()


def identify_engine(path: Path) -> EngineArtifact:
    engine = chess.engine.SimpleEngine.popen_uci(str(path))
    try:
        identity = dict(engine.id)
    finally:
        safe_quit(engine)
    return EngineArtifact(
        name=str(identity.get("name", path.stem)),
        author=str(identity.get("author", "")),
        path=str(path),
        sha256=sha256_file(path),
    )


def configure_engines(
    proton: chess.engine.SimpleEngine,
    stockfish: chess.engine.SimpleEngine,
    opponent_elo: int,
    hash_mb: int,
) -> None:
    proton.configure({"Hash": hash_mb, "UseBook": False, "HumanStyle": False})
    stockfish.configure({
        "Hash": hash_mb,
        "Threads": 1,
        "UCI_LimitStrength": True,
        "UCI_Elo": opponent_elo,
    })


def play_with_watchdog(
    engine: chess.engine.SimpleEngine,
    board: chess.Board,
    limit: chess.engine.Limit,
    game_token: object,
    timeout_seconds: float,
) -> chess.engine.PlayResult:
    coroutine = engine.protocol.play(board, limit, game=game_token)
    future = asyncio.run_coroutine_threadsafe(coroutine, engine.protocol.loop)
    try:
        return future.result(timeout=max(0.001, timeout_seconds))
    except concurrent.futures.TimeoutError as error:
        future.cancel()
        engine.close()
        raise EngineMoveTimeout("engine move watchdog expired") from error


def prepare_engine_for_game(
    engine: chess.engine.SimpleEngine,
    game_token: object,
    timeout_seconds: float = 10.0,
) -> None:
    async def prepare() -> None:
        # python-chess otherwise sends ucinewgame/isready inside the first
        # timed play call. Bind the token here so only position/go are clocked.
        engine.protocol.game = game_token
        engine.protocol._ucinewgame()
        await engine.protocol.ping()

    future = asyncio.run_coroutine_threadsafe(prepare(), engine.protocol.loop)
    try:
        future.result(timeout=max(0.001, timeout_seconds))
    except concurrent.futures.TimeoutError as error:
        future.cancel()
        engine.close()
        raise EngineMoveTimeout("engine game preparation timed out") from error


def play_game(
    proton: Any,
    stockfish: Any,
    opening: list[str],
    proton_is_white: bool,
    time_control: TimeControl,
    max_plies: int,
    game_token: object,
    *,
    player: PlayFunction = play_with_watchdog,
    clock: Callable[[], float] = time.monotonic,
) -> GameOutcome:
    board = chess.Board()
    for text in opening:
        board.push_uci(text)

    clocks = None if time_control.base_seconds is None else {
        chess.WHITE: time_control.base_seconds,
        chess.BLACK: time_control.base_seconds,
    }

    def finish(point: float, termination: str) -> GameOutcome:
        return GameOutcome(
            point=point,
            termination=termination,
            plies=board.ply(),
            moves=[move.uci() for move in board.move_stack],
            white_clock_seconds=None if clocks is None else round(max(0.0, clocks[chess.WHITE]), 6),
            black_clock_seconds=None if clocks is None else round(max(0.0, clocks[chess.BLACK]), 6),
        )

    while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
        side = board.turn
        proton_turn = side == (chess.WHITE if proton_is_white else chess.BLACK)
        active = proton if proton_turn else stockfish
        if clocks is not None and clocks[side] <= 0.0:
            return finish(0.0 if proton_turn else 1.0,
                          f"{'white' if side else 'black'} flag")

        limit = time_control.limit(clocks)
        watchdog = time_control.watchdog(side, clocks)
        started = clock()
        try:
            result = player(active, board, limit, game_token, watchdog)
        except EngineMoveTimeout:
            if clocks is not None:
                clocks[side] = 0.0
            return finish(0.0 if proton_turn else 1.0,
                          f"{'white' if side else 'black'} timeout")
        elapsed = max(0.0, clock() - started)

        if clocks is not None:
            clocks[side] -= elapsed
            if clocks[side] <= 0.0:
                clocks[side] = 0.0
                return finish(0.0 if proton_turn else 1.0,
                              f"{'white' if side else 'black'} flag")

        if result.move is None or result.move not in board.legal_moves:
            return finish(0.0 if proton_turn else 1.0,
                          f"{'white' if side else 'black'} illegal or no move")

        board.push(result.move)
        if clocks is not None:
            clocks[side] += time_control.increment_seconds

    outcome = board.outcome(claim_draw=True)
    if outcome is None:
        return finish(0.5, "move limit")
    termination = outcome.termination.name.lower().replace("_", " ")
    if outcome.winner is None:
        return finish(0.5, termination)
    proton_won = outcome.winner == (chess.WHITE if proton_is_white else chess.BLACK)
    return finish(1.0 if proton_won else 0.0, termination)


def run_level(
    proton_path: Path,
    stockfish_path: Path,
    opponent_elo: int,
    openings: list[list[str]],
    args: argparse.Namespace,
    time_control: TimeControl,
) -> MatchResult:
    games = args.games + args.games % 2
    pair_count = games // 2
    selected = select_openings(openings, pair_count, args.seed + opponent_elo)

    points: list[float] = []
    records: list[GameRecord] = []
    for pair_index, (opening_index, opening) in enumerate(selected, start=1):
        for proton_is_white in (True, False):
            proton = chess.engine.SimpleEngine.popen_uci(str(proton_path))
            stockfish = chess.engine.SimpleEngine.popen_uci(str(stockfish_path))
            try:
                game_token = (args.seed, opponent_elo, pair_index, proton_is_white)
                configure_engines(proton, stockfish, opponent_elo, args.hash)
                prepare_engine_for_game(proton, game_token)
                prepare_engine_for_game(stockfish, game_token)
                outcome = play_game(
                    proton,
                    stockfish,
                    opening,
                    proton_is_white,
                    time_control,
                    args.max_plies,
                    game_token,
                )
            finally:
                safe_quit(proton)
                safe_quit(stockfish)

            points.append(outcome.point)
            records.append(GameRecord(
                pair=pair_index,
                proton_color="white" if proton_is_white else "black",
                opening_index=opening_index,
                opening_moves=list(opening),
                point=outcome.point,
                termination=outcome.termination,
                plies=outcome.plies,
                moves=outcome.moves,
                white_clock_seconds=outcome.white_clock_seconds,
                black_clock_seconds=outcome.black_clock_seconds,
            ))
            print(
                f"elo={opponent_elo} pair={pair_index}/{pair_count} "
                f"proton={'white' if proton_is_white else 'black'} "
                f"result={outcome.point:g} termination={outcome.termination} "
                f"score={sum(points):g}/{len(points)}",
                flush=True,
            )

    wins = points.count(1.0)
    draws = points.count(0.5)
    losses = points.count(0.0)
    score = statistics.fmean(points)
    return MatchResult(
        opponent_elo=opponent_elo,
        games=len(points),
        wins=wins,
        draws=draws,
        losses=losses,
        score=score,
        estimated_elo=logistic_elo(opponent_elo, score),
        records=records,
    )


def build_time_control(args: argparse.Namespace) -> TimeControl:
    move_time = 0.05 if args.move_time is None and args.base_seconds is None else args.move_time
    if move_time is not None and move_time <= 0.0:
        raise ValueError("move-time must be positive")
    if args.base_seconds is not None and args.base_seconds <= 0.0:
        raise ValueError("base-seconds must be positive")
    if args.increment < 0.0:
        raise ValueError("increment must be non-negative")
    if args.base_seconds is None and args.increment != 0.0:
        raise ValueError("increment requires --base-seconds")
    if args.watchdog_grace <= 0.0:
        raise ValueError("watchdog-grace must be positive")
    return TimeControl(
        move_time=move_time,
        base_seconds=args.base_seconds,
        increment_seconds=args.increment,
        watchdog_grace_seconds=args.watchdog_grace,
    )


def main() -> int:
    args = parse_args()
    proton_path = args.proton.resolve()
    stockfish_path = args.stockfish.resolve()
    openings_path = args.openings.resolve()
    if not proton_path.is_file() or not stockfish_path.is_file():
        raise FileNotFoundError("both engine paths must be files")
    if not openings_path.is_file():
        raise FileNotFoundError(openings_path)
    if args.games < 2 or args.max_plies < 40 or args.hash < 1:
        raise ValueError("games, max-plies, or hash is outside its valid range")

    time_control = build_time_control(args)
    openings = load_openings(openings_path)
    tool_path = Path(__file__).resolve()
    report = MatchReport(
        schema_version=1,
        created_utc=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        seed=args.seed,
        requested_games_per_level=args.games,
        max_plies=args.max_plies,
        hash_mb=args.hash,
        time_control=time_control.payload(),
        proton=identify_engine(proton_path),
        stockfish=identify_engine(stockfish_path),
        openings=InputArtifact(path=str(openings_path), sha256=sha256_file(openings_path)),
        tool=InputArtifact(path=str(tool_path), sha256=sha256_file(tool_path)),
        git_revision=git_revision(tool_path.parents[1]),
        git_dirty=git_dirty(tool_path.parents[1]),
        python_version=platform.python_version(),
        python_chess_version=getattr(chess, "__version__", "unknown"),
        host={
            "platform": platform.platform(),
            "system": platform.system(),
            "release": platform.release(),
            "machine": platform.machine(),
            "processor": platform.processor(),
            "logical_cpu_count": os.cpu_count(),
        },
        proton_options={"Hash": args.hash, "UseBook": False, "HumanStyle": False},
        stockfish_options={
            "Hash": args.hash,
            "Threads": 1,
            "UCI_LimitStrength": True,
            "UCI_Elo": list(args.opponent_elo),
        },
        results=[
            run_level(proton_path, stockfish_path, rating, openings, args, time_control)
            for rating in args.opponent_elo
        ],
    )
    payload = asdict(report)
    rendered = json.dumps(payload, indent=2)
    print(rendered)
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (chess.engine.EngineError, chess.engine.EngineTerminatedError) as error:
        print(f"engine failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
