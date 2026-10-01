#!/usr/bin/env python3
"""Compare Proton move generation with python-chess on reproducible random positions."""

import argparse
from pathlib import Path
import random

import chess

from uci_smoke import EngineProcess


def perft(board: chess.Board, depth: int) -> int:
    if depth == 0:
        return 1
    total = 0
    for move in board.legal_moves:
        board.push(move)
        total += perft(board, depth - 1)
        board.pop()
    return total


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("--positions", type=int, default=128)
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--seed", type=int, default=20261001)
    args = parser.parse_args()
    if args.positions < 1 or not 1 <= args.depth <= 3:
        raise ValueError("positions must be positive and depth must be 1..3")
    rng = random.Random(args.seed)
    board = chess.Board()
    engine = EngineProcess(args.binary.resolve())
    try:
        engine.send("uci")
        engine.wait_for(lambda line: line == "uciok")
        for sample in range(args.positions):
            if board.is_game_over() or sample % 64 == 0:
                board.reset()
            for _ in range(rng.randint(1, 4)):
                moves = list(board.legal_moves)
                if not moves:
                    board.reset()
                    moves = list(board.legal_moves)
                board.push(rng.choice(moves))
            expected = perft(board, args.depth)
            fen = board.fen(en_passant="fen")
            engine.send(f"position fen {fen}")
            engine.send(f"perft {args.depth}")
            line, _ = engine.wait_for(lambda line: line.startswith("info string perft depth "))
            tokens = line.split()
            actual = int(tokens[tokens.index("nodes") + 1])
            if actual != expected:
                raise AssertionError(f"sample={sample} fen={fen} expected={expected} actual={actual}")
    finally:
        engine.close()
    print(f"Differential perft passed: {args.positions} positions, depth {args.depth}, seed {args.seed}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
