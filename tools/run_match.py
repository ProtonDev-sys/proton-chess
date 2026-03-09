from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import shlex

import chess
import chess.engine
import chess.pgn


@dataclass(slots=True)
class MatchScore:
    wins: int = 0
    losses: int = 0
    draws: int = 0

    def add(self, result: str, proton_is_white: bool) -> None:
        if result == "1-0":
            if proton_is_white:
                self.wins += 1
            else:
                self.losses += 1
        elif result == "0-1":
            if proton_is_white:
                self.losses += 1
            else:
                self.wins += 1
        else:
            self.draws += 1


def parse_option(option: str) -> tuple[str, object]:
    name, raw_value = option.split("=", 1)
    value: object = raw_value
    lowered = raw_value.lower()
    if lowered in {"true", "false"}:
        value = lowered == "true"
    else:
        try:
            value = int(raw_value)
        except ValueError:
            try:
                value = float(raw_value)
            except ValueError:
                value = raw_value
    return name, value


def play_game(white_engine, black_engine, limit: chess.engine.Limit, opening_moves: list[chess.Move]) -> tuple[str, chess.pgn.Game]:
    board = chess.Board()
    game = chess.pgn.Game()
    node = game

    for move in opening_moves:
        board.push(move)
        node = node.add_variation(move)

    while not board.is_game_over(claim_draw=False):
        engine = white_engine if board.turn == chess.WHITE else black_engine
        try:
            result = engine.play(board, limit)
        except TimeoutError:
            game.headers["Termination"] = "engine timeout"
            result = "0-1" if board.turn == chess.WHITE else "1-0"
            game.headers["Result"] = result
            return result, game
        board.push(result.move)
        node = node.add_variation(result.move)

        if board.fullmove_number > 300:
            break

    result = board.result(claim_draw=False)
    game.headers["Result"] = result
    return result, game


def parse_opening(line: str) -> list[chess.Move]:
    board = chess.Board()
    moves: list[chess.Move] = []
    for token in line.split():
        move = chess.Move.from_uci(token)
        if move not in board.legal_moves:
            raise ValueError(f"illegal opening move {token} in line {line}")
        board.push(move)
        moves.append(move)
    return moves


def main() -> None:
    parser = argparse.ArgumentParser(description="Play a small UCI match between Proton Chess and another engine.")
    parser.add_argument("proton", type=Path)
    parser.add_argument("opponent", type=Path)
    parser.add_argument("--games", type=int, default=4)
    parser.add_argument("--seconds", type=float, default=0.2)
    parser.add_argument("--increment", type=float, default=0.0)
    parser.add_argument("--openings", type=Path)
    parser.add_argument("--pgn", type=Path)
    parser.add_argument("--proton-option", action="append", default=[])
    parser.add_argument("--opponent-option", action="append", default=[])
    args = parser.parse_args()

    opening_lines = [""]
    if args.openings:
        opening_lines = [line.strip() for line in args.openings.read_text(encoding="utf-8").splitlines() if line.strip()]

    limit = chess.engine.Limit(time=args.seconds)
    score = MatchScore()
    games: list[chess.pgn.Game] = []

    proton_cmd = shlex.split(str(args.proton), posix=False)
    opponent_cmd = shlex.split(str(args.opponent), posix=False)

    with chess.engine.SimpleEngine.popen_uci(proton_cmd) as proton_engine, \
         chess.engine.SimpleEngine.popen_uci(opponent_cmd) as opponent_engine:
        proton_engine.configure({"Threads": 1, "Hash": 64})
        try:
            proton_engine.configure({"Backend": "cpu"})
        except chess.engine.EngineError:
            pass
        try:
            opponent_engine.configure({"Threads": 1, "Hash": 64})
        except chess.engine.EngineError:
            pass

        if args.proton_option:
            proton_engine.configure(dict(parse_option(option) for option in args.proton_option))
        if args.opponent_option:
            opponent_engine.configure(dict(parse_option(option) for option in args.opponent_option))

        for game_index in range(args.games):
            opening = parse_opening(opening_lines[game_index % len(opening_lines)])
            proton_is_white = game_index % 2 == 0
            white_engine = proton_engine if proton_is_white else opponent_engine
            black_engine = opponent_engine if proton_is_white else proton_engine
            result, game = play_game(white_engine, black_engine, limit, opening)
            game.headers["White"] = "ProtonChess" if proton_is_white else args.opponent.stem
            game.headers["Black"] = args.opponent.stem if proton_is_white else "ProtonChess"
            score.add(result, proton_is_white)
            games.append(game)
            print(f"game={game_index + 1} result={result} proton_white={proton_is_white}")

    if args.pgn:
        args.pgn.parent.mkdir(parents=True, exist_ok=True)
        with args.pgn.open("w", encoding="utf-8", newline="\n") as handle:
            for game in games:
                print(game, file=handle, end="\n\n")

    print(f"score wins={score.wins} losses={score.losses} draws={score.draws}")


if __name__ == "__main__":
    main()
