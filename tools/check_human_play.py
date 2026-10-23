#!/usr/bin/env python3
"""Check actual human bestmoves, reproducibility and fresh loss confirmation."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import chess
import chess.engine


ROOT = Path(__file__).resolve().parents[1]


def search(engine, board, depth):
    with engine.analysis(board, chess.engine.Limit(depth=depth), game=object(),
                         info=chess.engine.INFO_ALL) as analysis:
        infos = list(analysis)
        result = analysis.wait()
    completed = [info for info in infos if info.get("depth") == depth and info.get("pv")]
    assert completed, (board.fen(), infos)
    assert result.move in board.legal_moves, (board.fen(), result)
    assert completed[-1]["pv"][0] == result.move, (result, completed[-1])
    return result.move, completed[0], completed[-1], infos


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engine", type=Path)
    parser.add_argument("--positions", type=Path,
                        default=ROOT / "openings/uho_lichess_4852_v1_200.epd")
    parser.add_argument("--count", type=int, default=50)
    parser.add_argument("--seeds", type=int, default=32)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--skill", type=int, default=16, choices=range(21))
    parser.add_argument("--variety", type=int, default=60, choices=range(101))
    parser.add_argument("--no-book", action="store_true")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    if args.count < 1 or args.seeds < 1 or args.depth < 2:
        parser.error("count and seeds must be positive; depth must be at least two")
    binary = args.engine.resolve()
    fens = [line.split(";")[0].strip() for line in args.positions.read_text().splitlines()
            if line.strip() and not line.startswith("#")][:args.count]
    if not fens:
        parser.error("position suite is empty")
    allowance = min(700, 12 + (20 - args.skill) * 8 + (20 - args.skill) ** 2 * 2)
    summary = {"engine_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
               "suite_sha256": hashlib.sha256(args.positions.read_bytes()).hexdigest(),
               "positions": len(fens), "seeds": args.seeds, "depth": args.depth,
               "skill": args.skill, "variety": args.variety, "book": not args.no_book,
               "allowance_cp": allowance, "choices": 0, "alternatives": 0,
               "meaningful_losses": 0, "maximum_loss_cp": 0}
    records = []
    common = {"Hash": 1, "UseBook": False, "HumanStyle": False}
    with chess.engine.SimpleEngine.popen_uci(str(binary)) as human, \
            chess.engine.SimpleEngine.popen_uci(str(binary)) as verifier:
        verifier.configure(common)
        human.configure(common | {"HumanStyle": True, "HumanSkill": args.skill,
                                  "HumanMaxLossCp": 12, "HumanVariety": args.variety,
                                  "UseBook": not args.no_book, "BookRandomness": 70,
                                  "BookFile": str(ROOT / "openings/book_lines.txt")})
        for fen in fens:
            board = chess.Board(fen)
            for seed in range(1, args.seeds + 1):
                human.configure({"HumanSeed": seed})
                move, main_info, final_info, infos = search(human, board, args.depth)
                # Reassigning an unchanged seed intentionally retains RNG state.
                human.configure({"HumanSeed": seed + args.seeds + 1})
                human.configure({"HumanSeed": seed})
                repeat, _, repeated_info, _ = search(human, board, args.depth)
                assert move == repeat and final_info["score"] == repeated_info["score"], (fen, seed)
                summary["choices"] += 1
                if move == main_info["pv"][0]:
                    continue
                checked = verifier.analyse(board, chess.engine.Limit(depth=args.depth),
                                           root_moves=[move], game=object())
                assert checked["score"] == final_info["score"], (fen, seed, checked, final_info)
                best = main_info["score"].pov(board.turn).score()
                chosen = checked["score"].pov(board.turn).score()
                assert best is not None, "human selection changed a searched mate"
                if chosen is None:
                    assert checked["score"].pov(board.turn).mate() > 0, "selected a forced loss"
                loss = best - chosen if chosen is not None else -32000
                assert loss <= allowance, (fen, seed, move, best, chosen)
                if loss > 25:
                    shallow = next(info for info in infos if info.get("depth") == 2)
                    shallow_best = shallow["score"].pov(board.turn).score()
                    short = verifier.analyse(board, chess.engine.Limit(depth=2),
                                             root_moves=[move], game=object())
                    shallow_chosen = short["score"].pov(board.turn).score()
                    assert shallow_best is not None and shallow_chosen is not None
                    assert shallow_chosen >= shallow_best - 35, (fen, seed, shallow_best, shallow_chosen)
                    summary["meaningful_losses"] += 1
                summary["alternatives"] += 1
                summary["maximum_loss_cp"] = max(summary["maximum_loss_cp"], loss)
                records.append({"fen": fen, "seed": seed, "move": move.uci(), "loss_cp": loss})
    print(json.dumps(summary, indent=2))
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(json.dumps({"summary": summary, "alternatives": records}, indent=2) + "\n")


if __name__ == "__main__":
    main()
