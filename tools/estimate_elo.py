#!/usr/bin/env python3
"""Estimate Proton's playing strength against Stockfish UCI_Elo levels."""

from __future__ import annotations

import argparse
import json
import math
import random
import statistics
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

import chess
import chess.engine


@dataclass
class MatchResult:
    opponent_elo: int
    games: int
    wins: int
    draws: int
    losses: int
    score: float
    estimated_elo: float
    ci95_low: float
    ci95_high: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("proton", type=Path)
    parser.add_argument("stockfish", type=Path)
    parser.add_argument("--opponent-elo", type=int, action="append", required=True)
    parser.add_argument("--games", type=int, default=20,
                        help="games per opponent level; rounded up to an even number")
    parser.add_argument("--move-time", type=float, default=0.05,
                        help="seconds allocated to each engine per move")
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


def logistic_elo(opponent_elo: int, score: float) -> float:
    score = min(0.999, max(0.001, score))
    return opponent_elo + 400.0 * math.log10(score / (1.0 - score))


def confidence_interval(opponent_elo: int, points: list[float]) -> tuple[float, float]:
    if len(points) < 2:
        return float("-inf"), float("inf")
    mean = statistics.fmean(points)
    standard_error = statistics.stdev(points) / math.sqrt(len(points))
    return (
        logistic_elo(opponent_elo, mean - 1.96 * standard_error),
        logistic_elo(opponent_elo, mean + 1.96 * standard_error),
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


def play_game(
    proton: chess.engine.SimpleEngine,
    stockfish: chess.engine.SimpleEngine,
    opening: list[str],
    proton_is_white: bool,
    move_time: float,
    max_plies: int,
) -> float:
    board = chess.Board()
    for text in opening:
        board.push_uci(text)

    while not board.is_game_over(claim_draw=True) and board.ply() < max_plies:
        proton_turn = board.turn == chess.WHITE if proton_is_white else board.turn == chess.BLACK
        active = proton if proton_turn else stockfish
        result = active.play(board, chess.engine.Limit(time=move_time))
        if result.move is None or result.move not in board.legal_moves:
            return 0.0 if proton_turn else 1.0
        board.push(result.move)

    outcome = board.outcome(claim_draw=True)
    if outcome is None or outcome.winner is None:
        return 0.5
    proton_won = outcome.winner == (chess.WHITE if proton_is_white else chess.BLACK)
    return 1.0 if proton_won else 0.0


def run_level(
    proton_path: Path,
    stockfish_path: Path,
    opponent_elo: int,
    openings: list[list[str]],
    args: argparse.Namespace,
) -> MatchResult:
    games = args.games + args.games % 2
    pair_count = games // 2
    rng = random.Random(args.seed + opponent_elo)
    selected = rng.sample(openings, k=min(pair_count, len(openings)))
    while len(selected) < pair_count:
        selected.append(rng.choice(openings))

    proton = chess.engine.SimpleEngine.popen_uci(str(proton_path))
    stockfish = chess.engine.SimpleEngine.popen_uci(str(stockfish_path))
    points: list[float] = []
    try:
        configure_engines(proton, stockfish, opponent_elo, args.hash)
        for pair_index, opening in enumerate(selected, start=1):
            for proton_is_white in (True, False):
                point = play_game(
                    proton,
                    stockfish,
                    opening,
                    proton_is_white,
                    args.move_time,
                    args.max_plies,
                )
                points.append(point)
                print(
                    f"elo={opponent_elo} pair={pair_index}/{pair_count} "
                    f"proton={'white' if proton_is_white else 'black'} "
                    f"result={point:g} score={sum(points):g}/{len(points)}",
                    flush=True,
                )
    finally:
        proton.quit()
        stockfish.quit()

    wins = points.count(1.0)
    draws = points.count(0.5)
    losses = points.count(0.0)
    score = statistics.fmean(points)
    ci_low, ci_high = confidence_interval(opponent_elo, points)
    return MatchResult(
        opponent_elo=opponent_elo,
        games=len(points),
        wins=wins,
        draws=draws,
        losses=losses,
        score=score,
        estimated_elo=logistic_elo(opponent_elo, score),
        ci95_low=ci_low,
        ci95_high=ci_high,
    )


def main() -> int:
    args = parse_args()
    proton_path = args.proton.resolve()
    stockfish_path = args.stockfish.resolve()
    if not proton_path.is_file() or not stockfish_path.is_file():
        raise FileNotFoundError("both engine paths must be files")
    if args.games < 2 or args.move_time <= 0 or args.max_plies < 40:
        raise ValueError("games, move-time, or max-plies is outside its valid range")

    openings = load_openings(args.openings.resolve())
    results = [
        run_level(proton_path, stockfish_path, rating, openings, args)
        for rating in args.opponent_elo
    ]
    payload = [asdict(result) for result in results]
    print(json.dumps(payload, indent=2))
    if args.json_path:
        args.json_path.parent.mkdir(parents=True, exist_ok=True)
        args.json_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (chess.engine.EngineError, chess.engine.EngineTerminatedError) as error:
        print(f"engine failure: {error}", file=sys.stderr)
        raise SystemExit(1) from error
