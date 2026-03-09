from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import random
import subprocess

import chess


@dataclass(slots=True)
class Result:
    fen: str
    ours: list[str]
    reference: list[str]


def engine_moves(engine_path: Path, fen: str) -> list[str]:
    command = f"position fen {fen}\nmoves\nquit\n"
    proc = subprocess.run(
        [str(engine_path)],
        input=command,
        text=True,
        capture_output=True,
        check=True,
    )
    return sorted(line.strip() for line in proc.stdout.splitlines() if line.strip())


def random_positions(count: int, plies: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    fens: list[str] = []
    for _ in range(count):
        board = chess.Board()
        for _ in range(plies):
            if board.is_game_over():
                break
            moves = list(board.legal_moves)
            board.push(rng.choice(moves))
        fens.append(board.fen())
    return fens


def check_positions(engine_path: Path, fens: list[str]) -> list[Result]:
    mismatches: list[Result] = []
    for fen in fens:
        ours = engine_moves(engine_path, fen)
        reference = sorted(move.uci() for move in chess.Board(fen).legal_moves)
        if ours != reference:
            mismatches.append(Result(fen=fen, ours=ours, reference=reference))
    return mismatches


def main() -> None:
    parser = argparse.ArgumentParser(description="Cross-check Proton Chess legal moves against python-chess.")
    parser.add_argument("engine", type=Path)
    parser.add_argument("--count", type=int, default=25)
    parser.add_argument("--plies", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    args = parser.parse_args()

    mismatches = check_positions(args.engine, random_positions(args.count, args.plies, args.seed))
    if mismatches:
        for mismatch in mismatches[:5]:
            print(f"FEN: {mismatch.fen}")
            print(f"OURS: {mismatch.ours}")
            print(f"REF:  {mismatch.reference}")
        raise SystemExit(1)

    print(f"crosscheck passed for {args.count} positions at plies={args.plies}")


if __name__ == "__main__":
    main()
