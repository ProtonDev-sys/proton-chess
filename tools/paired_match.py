#!/usr/bin/env python3
"""Run independent, color-reversed opening pairs against a UCI engine."""

from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import ExitStack
from dataclasses import asdict, dataclass
import hashlib
import json
import math
from pathlib import Path
import random
import time

import chess
import chess.engine
import chess.pgn

from match_stats import score_bounds


@dataclass
class GameResult:
    pair: int
    candidate_white: bool
    point: float | None
    termination: str
    plies: int
    pgn: str


def load_openings(path: Path) -> list[list[str]]:
    openings = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        moves = line.split("|", 1)[-1].split()
        board = chess.Board()
        try:
            for move in moves:
                board.push_uci(move)
        except ValueError as error:
            raise ValueError(f"invalid opening at {path}:{line_number}") from error
        if board.is_game_over(claim_draw=True):
            raise ValueError(f"terminal opening at {path}:{line_number}")
        openings.append(moves)
    if not openings:
        raise ValueError(f"no openings in {path}")
    return openings


def configure_engine(engine: chess.engine.SimpleEngine, hash_mb: int) -> dict:
    settings = {"Hash": hash_mb, "Threads": 1, "UseBook": False,
                "UCI_LimitStrength": False, "Skill Level": 20,
                "HumanStyle": False, "Move Overhead": 1, "MoveOverhead": 1}
    applied = {name: value for name, value in settings.items() if name in engine.options}
    engine.configure(applied)
    return applied


def play_game(candidate: chess.engine.SimpleEngine, opponent: chess.engine.SimpleEngine,
              opening: list[str], candidate_white: bool, args: argparse.Namespace,
              pair: int, level: int | None) -> GameResult:
    board = chess.Board()
    for move in opening:
        board.push_uci(move)
    game_token = object()
    while board.ply() < args.max_plies and board.outcome(claim_draw=True) is None:
        candidate_turn = board.turn == (chess.WHITE if candidate_white else chess.BLACK)
        engine = candidate if candidate_turn else opponent
        result = engine.play(board, chess.engine.Limit(time=args.move_time), game=game_token)
        if result.move is None or result.move not in board.legal_moves:
            raise RuntimeError(f"illegal/missing move by {engine.id} in {board.fen()}")
        board.push(result.move)
    outcome = board.outcome(claim_draw=True)
    point = None if outcome is None else (
        0.5 if outcome.winner is None else
        float(outcome.winner == (chess.WHITE if candidate_white else chess.BLACK)))
    termination = "ply-limit" if outcome is None else outcome.termination.name.lower()
    game = chess.pgn.Game.from_board(board)
    candidate_name = candidate.id.get("name", "candidate")
    opponent_name = opponent.id.get("name", "opponent")
    game.headers.update({"Event": "Proton paired strength test", "Round": str(pair + 1),
                         "White": candidate_name if candidate_white else opponent_name,
                         "Black": opponent_name if candidate_white else candidate_name,
                         "Result": "*" if outcome is None else outcome.result(),
                         "Termination": termination, "TimeControl": f"movetime {args.move_time}"})
    if level is not None:
        game.headers["BlackElo" if candidate_white else "WhiteElo"] = str(level)
    return GameResult(pair, candidate_white, point, termination, board.ply(), str(game))


def play_pair(candidate_path: Path, opponent_path: Path, level: int | None,
              pair: int, opening: list[str], args: argparse.Namespace) -> list[GameResult]:
    with ExitStack() as stack:
        candidate = stack.enter_context(chess.engine.SimpleEngine.popen_uci(str(candidate_path)))
        opponent = stack.enter_context(chess.engine.SimpleEngine.popen_uci(str(opponent_path)))
        configure_engine(candidate, args.hash)
        configure_engine(opponent, args.hash)
        if level is not None:
            option = opponent.options.get("UCI_Elo")
            if option is None or not option.min <= level <= option.max:
                raise ValueError(f"opponent does not support UCI_Elo={level}: {option}")
            opponent.configure({"UCI_LimitStrength": True, "UCI_Elo": level})
        elif "UCI_LimitStrength" in opponent.options:
            opponent.configure({"UCI_LimitStrength": False})
        return [play_game(candidate, opponent, opening, candidate_white, args, pair, level)
                for candidate_white in (True, False)]


def summarize(results: list[GameResult], level: int | None, confidence: float = 0.95) -> dict:
    pairs = {}
    for game in results:
        colors = pairs.setdefault(game.pair, set())
        if game.candidate_white in colors:
            raise ValueError("duplicate game in opening pair")
        colors.add(game.candidate_white)
    if any(colors != {True, False} for colors in pairs.values()):
        raise ValueError("incomplete opening pair")
    results = sorted(results, key=lambda game: (game.pair, not game.candidate_white))
    points = [game.point for game in results]
    completed = [point for point in points if point is not None]
    lower, upper = score_bounds(points, confidence)
    return {"opponent_elo": level, "games": len(points), "pairs": len(points) // 2,
            "wins": points.count(1.0), "draws": points.count(0.5), "losses": points.count(0.0),
            "unresolved": points.count(None),
            "completed_score": sum(completed) / len(completed) if completed else None,
            "pessimistic_score": sum(completed) / len(points),
            "confidence": confidence,
            "score_lower_one_sided": lower, "score_upper_one_sided": upper,
            "confidence_method": "Hoeffding on opening pairs; unresolved=0/1 for lower/upper bounds",
            "passes": lower > 0.5 and None not in points}


def engine_identity(path: Path, hash_mb: int) -> dict:
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    with chess.engine.SimpleEngine.popen_uci(str(path)) as engine:
        return {"path": str(path), "sha256": digest, "id": engine.id,
                "configured_options": configure_engine(engine, hash_mb),
                "options": {name: asdict(option) for name, option in engine.options.items()}}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("proton", type=Path)
    parser.add_argument("stockfish", type=Path)
    parser.add_argument("--opponent-elo", type=int, action="append")
    parser.add_argument("--games", type=int, default=40)
    parser.add_argument("--move-time", type=float, default=0.1)
    parser.add_argument("--max-plies", type=int, default=800)
    parser.add_argument("--hash", type=int, default=64)
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--seed", type=int, default=20261001)
    parser.add_argument("--confidence", type=float, default=0.95)
    parser.add_argument("--openings", type=Path, default=Path("openings/match_lines.txt"))
    parser.add_argument("--json", type=Path, dest="json_path")
    parser.add_argument("--pgn", type=Path, dest="pgn_path")
    return parser.parse_args()


def write_json(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def main() -> int:
    args = parse_args()
    if args.games < 2 or args.games % 2 or args.workers < 1 or args.hash < 1:
        raise ValueError("games must be positive and even; workers/hash must be positive")
    if not math.isfinite(args.move_time) or args.move_time <= 0 or args.max_plies < 40:
        raise ValueError("invalid move-time or ply limit")
    if not 0 < args.confidence < 1:
        raise ValueError("confidence must be between zero and one")
    if args.pgn_path and args.pgn_path.exists():
        raise FileExistsError(f"refusing to mix matches in {args.pgn_path}")
    candidate_path, opponent_path = args.proton.resolve(), args.stockfish.resolve()
    openings = load_openings(args.openings.resolve())
    rng = random.Random(args.seed)
    selected = [rng.choice(openings) for _ in range(args.games // 2)]
    metadata = {"candidate": engine_identity(candidate_path, args.hash),
                "opponent": engine_identity(opponent_path, args.hash),
                "settings": {"games": args.games, "move_time": args.move_time,
                             "max_plies": args.max_plies, "hash_mb": args.hash,
                             "threads_per_engine": 1, "move_overhead_ms": 1,
                             "workers": args.workers, "seed": args.seed,
                             "confidence": args.confidence,
                             "openings_sha256": hashlib.sha256(args.openings.read_bytes()).hexdigest()},
                "status": "running", "results": [], "records": []}
    started = time.monotonic()
    try:
        for level in args.opponent_elo or []:
            option = metadata["opponent"]["options"].get("UCI_Elo")
            if option is None or not option["min"] <= level <= option["max"]:
                raise ValueError(f"opponent does not support UCI_Elo={level}: {option}")
        for level in args.opponent_elo or [None]:
            results = []
            with ThreadPoolExecutor(max_workers=args.workers) as pool:
                futures = [pool.submit(play_pair, candidate_path, opponent_path, level, pair, opening, args)
                           for pair, opening in enumerate(selected)]
                for future in as_completed(futures):
                    pair_results = future.result()
                    results.extend(pair_results)
                    metadata["records"].extend({**asdict(game), "opponent_elo": level}
                                               for game in pair_results)
                    print(f"level={level or 'full'} pairs={len(results) // 2}/{len(selected)} "
                          f"points={sum(game.point or 0 for game in results):g}/{len(results)} "
                          f"last={[game.point for game in pair_results]}", flush=True)
                    if args.pgn_path:
                        args.pgn_path.parent.mkdir(parents=True, exist_ok=True)
                        with args.pgn_path.open("a", encoding="utf-8") as output:
                            for game in pair_results:
                                output.write(game.pgn + "\n\n")
                    if args.json_path:
                        write_json(args.json_path, metadata)
            summary = summarize(results, level, args.confidence)
            metadata["results"].append(summary)
            print(json.dumps(summary, indent=2), flush=True)
        metadata["status"] = "completed"
    except Exception as error:
        metadata["status"] = "failed"
        metadata["error"] = str(error)
        raise
    finally:
        metadata["elapsed_seconds"] = round(time.monotonic() - started, 3)
        if args.json_path:
            write_json(args.json_path, metadata)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
