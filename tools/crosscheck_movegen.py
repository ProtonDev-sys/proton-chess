from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import random
import re
import subprocess

import chess


@dataclass(slots=True)
class Result:
    fen: str
    ours: list[str]
    reference: list[str]


UCI_MOVE = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$", re.IGNORECASE)


def engine_moves(engine_path: Path, fen: str) -> list[str]:
    command = f"uci\nisready\nposition fen {fen}\nmoves\nquit\n"
    proc = subprocess.run(
        [str(engine_path)],
        input=command,
        text=True,
        capture_output=True,
        check=True,
        timeout=10,
    )
    return sorted(
        line.strip().lower()
        for line in proc.stdout.splitlines()
        if UCI_MOVE.fullmatch(line.strip())
    )


def load_fens(path: Path) -> list[str]:
    with path.open("r", encoding="utf-8") as handle:
        return [
            line
            for raw_line in handle
            if (line := raw_line.strip()) and not line.startswith("#")
        ]


def random_positions(count: int, plies: int, seed: int) -> list[str]:
    rng = random.Random(seed)
    fens: list[str] = []
    for _ in range(count):
        board = chess.Board()
        for _ in range(plies):
            if board.is_game_over():
                break
            board.push(rng.choice(list(board.legal_moves)))
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
    parser = argparse.ArgumentParser(
        description="Cross-check Proton Chess legal moves against python-chess."
    )
    parser.add_argument("engine", type=Path)
    parser.add_argument("--count", type=int, default=25)
    parser.add_argument("--plies", type=int, default=10)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--fen-file", type=Path, default=Path("tests/movegen_positions.txt"))
    args = parser.parse_args()

    if not args.engine.is_file():
        raise FileNotFoundError(args.engine)
    if args.count < 0 or args.plies < 0:
        raise ValueError("count and plies must be non-negative")

    fens = load_fens(args.fen_file) if args.fen_file.exists() else []
    curated = len(fens)
    fens.extend(random_positions(args.count, args.plies, args.seed))

    mismatches = check_positions(args.engine, fens)
    if mismatches:
        for mismatch in mismatches[:5]:
            print(f"FEN: {mismatch.fen}")
            print(f"OURS: {mismatch.ours}")
            print(f"REF:  {mismatch.reference}")
        raise SystemExit(1)

    print(
        f"crosscheck passed for {len(fens)} positions "
        f"(curated={curated}, random={args.count}, plies={args.plies}, seed={args.seed})"
    )


if __name__ == "__main__":
    main()
