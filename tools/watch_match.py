from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
from queue import Empty, Queue
import shlex
import threading
import tkinter as tk
from tkinter import messagebox, ttk
from typing import Any

import chess
import chess.engine
import chess.pgn


PIECE_GLYPHS = {
    "P": "\u2659",
    "N": "\u2658",
    "B": "\u2657",
    "R": "\u2656",
    "Q": "\u2655",
    "K": "\u2654",
    "p": "\u265F",
    "n": "\u265E",
    "b": "\u265D",
    "r": "\u265C",
    "q": "\u265B",
    "k": "\u265A",
}

LIGHT_SQUARE = "#f0d9b5"
DARK_SQUARE = "#b58863"
LAST_MOVE_FILL = "#f6f669"
BOARD_PIXELS = 640
BOARD_MARGIN = 24
SQUARE_PIXELS = BOARD_PIXELS // 8


@dataclass(slots=True)
class MatchScore:
    proton_wins: int = 0
    opponent_wins: int = 0
    draws: int = 0

    def add(self, result: str, proton_is_white: bool) -> None:
        if result == "1-0":
            if proton_is_white:
                self.proton_wins += 1
            else:
                self.opponent_wins += 1
        elif result == "0-1":
            if proton_is_white:
                self.opponent_wins += 1
            else:
                self.proton_wins += 1
        else:
            self.draws += 1


def parse_option(option: str) -> tuple[str, object]:
    name, raw_value = option.split("=", 1)
    lowered = raw_value.lower()
    if lowered in {"true", "false"}:
        return name, lowered == "true"
    try:
        return name, int(raw_value)
    except ValueError:
        try:
            return name, float(raw_value)
        except ValueError:
            return name, raw_value


def parse_opening(line: str) -> list[chess.Move]:
    board = chess.Board()
    opening_moves: list[chess.Move] = []
    for token in line.split():
        move = chess.Move.from_uci(token)
        if move not in board.legal_moves:
            raise ValueError(f"illegal opening move {token} in line {line}")
        board.push(move)
        opening_moves.append(move)
    return opening_moves


def command_parts(command: str) -> list[str]:
    return shlex.split(command, posix=False)


class MatchWatcher:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.stop_event = threading.Event()
        self.events: Queue[dict[str, Any]] = Queue()
        self.board = chess.Board()
        self.score = MatchScore()
        self.moves: list[str] = []
        self.current_game = 0
        self.last_move: tuple[int, int] | None = None

        self.root = tk.Tk()
        self.root.title("Proton Chess Match Watcher")
        self.root.geometry("1180x760")
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.root.columnconfigure(0, weight=0)
        self.root.columnconfigure(1, weight=1)
        self.root.rowconfigure(0, weight=1)

        self.canvas = tk.Canvas(
            self.root,
            width=BOARD_PIXELS + (BOARD_MARGIN * 2),
            height=BOARD_PIXELS + (BOARD_MARGIN * 2),
            highlightthickness=0,
            bg="#1f1f1f",
        )
        self.canvas.grid(row=0, column=0, sticky="nsew", padx=(12, 0), pady=12)

        sidebar = ttk.Frame(self.root, padding=12)
        sidebar.grid(row=0, column=1, sticky="nsew")
        sidebar.columnconfigure(0, weight=1)
        sidebar.rowconfigure(2, weight=1)

        self.title_var = tk.StringVar(value="Starting engines...")
        self.score_var = tk.StringVar(value="Score Proton 0 | Opponent 0 | Draws 0")
        self.status_var = tk.StringVar(value="Waiting for first position...")

        ttk.Label(sidebar, textvariable=self.title_var, font=("Segoe UI", 18, "bold")).grid(
            row=0, column=0, sticky="w"
        )
        ttk.Label(sidebar, textvariable=self.score_var, font=("Segoe UI", 12)).grid(
            row=1, column=0, sticky="w", pady=(6, 8)
        )

        self.moves_box = tk.Text(sidebar, width=44, height=28, font=("Consolas", 11), state="disabled")
        self.moves_box.grid(row=2, column=0, sticky="nsew")

        ttk.Label(sidebar, textvariable=self.status_var, wraplength=420, font=("Segoe UI", 11)).grid(
            row=3, column=0, sticky="we", pady=(10, 0)
        )

        self.draw_board()
        self.worker = threading.Thread(target=self.run_match, daemon=True)

    def start(self) -> None:
        self.worker.start()
        self.root.after(100, self.poll_events)
        self.root.mainloop()

    def close(self) -> None:
        self.stop_event.set()
        self.root.destroy()

    def poll_events(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                self.handle_event(event)
        except Empty:
            pass
        if not self.stop_event.is_set():
            self.root.after(100, self.poll_events)

    def handle_event(self, event: dict[str, Any]) -> None:
        kind = event["kind"]
        if kind == "game_start":
            self.current_game = event["game_index"]
            self.board = chess.Board()
            self.moves = []
            self.last_move = None
            self.title_var.set(
                f"Game {event['game_index']}/{self.args.games}: {event['white_name']} vs {event['black_name']}"
            )
            self.status_var.set("Applying opening moves..." if event["opening_moves"] else "Thinking...")
            self.set_moves_text("")
            self.draw_board()
        elif kind == "opening_move":
            move = chess.Move.from_uci(event["uci"])
            san = self.board.san(move)
            self.board.push(move)
            self.moves.append(san)
            self.last_move = (move.from_square, move.to_square)
            self.refresh_moves()
            self.draw_board()
        elif kind == "move":
            move = chess.Move.from_uci(event["uci"])
            san = self.board.san(move)
            self.board.push(move)
            self.moves.append(san)
            self.last_move = (move.from_square, move.to_square)
            side = "White" if event["turn"] == "white" else "Black"
            self.status_var.set(f"{side} played {san}. Ply {event['ply']}.")
            self.refresh_moves()
            self.draw_board()
        elif kind == "game_end":
            self.score = event["score"]
            self.score_var.set(
                f"Score Proton {self.score.proton_wins} | Opponent {self.score.opponent_wins} | Draws {self.score.draws}"
            )
            self.status_var.set(f"Result {event['result']}. {event['termination']}")
        elif kind == "finished":
            self.status_var.set(event["message"])
        elif kind == "error":
            self.status_var.set(event["message"])
            messagebox.showerror("Match watcher error", event["message"])
            self.stop_event.set()

    def set_moves_text(self, text: str) -> None:
        self.moves_box.configure(state="normal")
        self.moves_box.delete("1.0", tk.END)
        self.moves_box.insert("1.0", text)
        self.moves_box.see(tk.END)
        self.moves_box.configure(state="disabled")

    def refresh_moves(self) -> None:
        rows: list[str] = []
        for index in range(0, len(self.moves), 2):
            move_number = (index // 2) + 1
            white = self.moves[index]
            black = self.moves[index + 1] if index + 1 < len(self.moves) else ""
            rows.append(f"{move_number:>3}. {white:<8} {black}")
        self.set_moves_text("\n".join(rows))

    def draw_board(self) -> None:
        self.canvas.delete("all")
        for rank in range(8):
            for file in range(8):
                square = chess.square(file, 7 - rank)
                x0 = BOARD_MARGIN + (file * SQUARE_PIXELS)
                y0 = BOARD_MARGIN + (rank * SQUARE_PIXELS)
                x1 = x0 + SQUARE_PIXELS
                y1 = y0 + SQUARE_PIXELS
                fill = LIGHT_SQUARE if (rank + file) % 2 == 0 else DARK_SQUARE
                if self.last_move and square in self.last_move:
                    fill = LAST_MOVE_FILL
                self.canvas.create_rectangle(x0, y0, x1, y1, fill=fill, outline=fill)

                piece = self.board.piece_at(square)
                if piece is not None:
                    self.canvas.create_text(
                        (x0 + x1) / 2,
                        (y0 + y1) / 2,
                        text=PIECE_GLYPHS[piece.symbol()],
                        font=("Segoe UI Symbol", 42),
                        fill="#111111" if piece.color == chess.BLACK else "#f9f9f9",
                    )

        for file in range(8):
            label = chr(ord("a") + file)
            x = BOARD_MARGIN + (file * SQUARE_PIXELS) + 8
            self.canvas.create_text(x, BOARD_MARGIN + BOARD_PIXELS + 10, text=label, anchor="sw", fill="white")
        for rank in range(8):
            label = str(8 - rank)
            y = BOARD_MARGIN + (rank * SQUARE_PIXELS) + 8
            self.canvas.create_text(8, y, text=label, anchor="nw", fill="white")

    def run_match(self) -> None:
        opening_lines = [""]
        if self.args.openings:
            opening_lines = [
                line.strip()
                for line in self.args.openings.read_text(encoding="utf-8").splitlines()
                if line.strip()
            ]

        proton_name = self.args.proton_name
        opponent_name = self.args.opponent_name or Path(command_parts(self.args.opponent)[0]).stem
        score = MatchScore()
        games: list[chess.pgn.Game] = []

        proton_cmd = command_parts(self.args.proton)
        opponent_cmd = command_parts(self.args.opponent)
        limit = chess.engine.Limit(time=self.args.seconds)

        try:
            with chess.engine.SimpleEngine.popen_uci(proton_cmd) as proton_engine, chess.engine.SimpleEngine.popen_uci(
                opponent_cmd
            ) as opponent_engine:
                proton_engine.configure({"Threads": 1, "Hash": 64})
                try:
                    proton_engine.configure({"Backend": "cpu"})
                except chess.engine.EngineError:
                    pass
                try:
                    opponent_engine.configure({"Threads": 1, "Hash": 64})
                except chess.engine.EngineError:
                    pass

                if self.args.proton_option:
                    proton_engine.configure(dict(parse_option(option) for option in self.args.proton_option))
                if self.args.opponent_option:
                    opponent_engine.configure(dict(parse_option(option) for option in self.args.opponent_option))

                for game_index in range(self.args.games):
                    if self.stop_event.is_set():
                        break

                    opening_moves = parse_opening(opening_lines[game_index % len(opening_lines)])
                    proton_is_white = game_index % 2 == 0
                    white_name = proton_name if proton_is_white else opponent_name
                    black_name = opponent_name if proton_is_white else proton_name
                    white_engine = proton_engine if proton_is_white else opponent_engine
                    black_engine = opponent_engine if proton_is_white else proton_engine

                    self.events.put(
                        {
                            "kind": "game_start",
                            "game_index": game_index + 1,
                            "white_name": white_name,
                            "black_name": black_name,
                            "opening_moves": len(opening_moves),
                        }
                    )

                    board = chess.Board()
                    game = chess.pgn.Game()
                    game.headers["Event"] = "Proton Chess Visual Match"
                    game.headers["White"] = white_name
                    game.headers["Black"] = black_name
                    node = game
                    final_result: str | None = None
                    termination = "Game finished."

                    for move in opening_moves:
                        if self.stop_event.is_set():
                            break
                        board.push(move)
                        node = node.add_variation(move)
                        self.events.put({"kind": "opening_move", "uci": move.uci()})
                    if self.stop_event.is_set():
                        break

                    while not board.is_game_over(claim_draw=False):
                        engine = white_engine if board.turn == chess.WHITE else black_engine
                        turn = "white" if board.turn == chess.WHITE else "black"
                        try:
                            result = engine.play(board, limit)
                        except TimeoutError:
                            final_result = "0-1" if board.turn == chess.WHITE else "1-0"
                            termination = "Engine timeout."
                            break

                        node = node.add_variation(result.move)
                        board.push(result.move)
                        self.events.put(
                            {
                                "kind": "move",
                                "uci": result.move.uci(),
                                "ply": board.ply(),
                                "turn": turn,
                            }
                        )

                        if board.fullmove_number > self.args.max_fullmoves:
                            final_result = "1/2-1/2"
                            termination = f"Stopped at move {self.args.max_fullmoves}."
                            break

                    if self.stop_event.is_set():
                        break

                    if final_result is None:
                        final_result = board.result(claim_draw=False)

                    game.headers["Result"] = final_result
                    game.headers["Termination"] = termination
                    score.add(final_result, proton_is_white)
                    games.append(game)
                    self.events.put(
                        {
                            "kind": "game_end",
                            "result": final_result,
                            "termination": termination,
                            "score": score,
                        }
                    )

                if self.args.pgn and games:
                    self.args.pgn.parent.mkdir(parents=True, exist_ok=True)
                    with self.args.pgn.open("w", encoding="utf-8", newline="\n") as handle:
                        for game in games:
                            print(game, file=handle, end="\n\n")
        except Exception as exc:  # pragma: no cover - surfaced in the UI
            self.events.put({"kind": "error", "message": str(exc)})
            return

        if self.stop_event.is_set():
            self.events.put({"kind": "finished", "message": "Stopped."})
            return

        summary = f"Match complete. Proton {score.proton_wins}, Opponent {score.opponent_wins}, Draws {score.draws}."
        if self.args.pgn and games:
            summary = f"{summary} PGN saved to {self.args.pgn}."
        self.events.put({"kind": "finished", "message": summary})


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Watch Proton Chess play another UCI engine in a simple local GUI.")
    parser.add_argument("proton", help="Path to the Proton Chess executable or command.")
    parser.add_argument("opponent", help="Path to the opposing UCI engine executable or command.")
    parser.add_argument("--games", type=int, default=2, help="Number of games to play.")
    parser.add_argument("--seconds", type=float, default=0.2, help="Fixed think time per move.")
    parser.add_argument("--openings", type=Path, help="Text file with one UCI opening line per row.")
    parser.add_argument("--pgn", type=Path, help="Optional PGN output path.")
    parser.add_argument("--max-fullmoves", type=int, default=300, help="Stop games after this many full moves.")
    parser.add_argument("--proton-name", default="ProtonChess", help="Display name for the Proton engine.")
    parser.add_argument("--opponent-name", help="Display name for the opposing engine.")
    parser.add_argument("--proton-option", action="append", default=[], help="Extra Proton UCI option, e.g. Threads=4.")
    parser.add_argument(
        "--opponent-option", action="append", default=[], help="Extra opponent UCI option, e.g. UCI_Elo=1800."
    )
    return parser


def main() -> None:
    watcher = MatchWatcher(build_parser().parse_args())
    watcher.start()


if __name__ == "__main__":
    main()
