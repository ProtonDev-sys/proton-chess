from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from queue import Empty, Queue
import shlex
import threading
import tkinter as tk
from tkinter import messagebox, simpledialog, ttk
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
SELECTED_FILL = "#7ec8e3"
TARGET_FILL = "#1f7a1f"
BOARD_PIXELS = 640
BOARD_MARGIN = 24
SQUARE_PIXELS = BOARD_PIXELS // 8

PROMOTION_MAP = {
    "q": chess.QUEEN,
    "r": chess.ROOK,
    "b": chess.BISHOP,
    "n": chess.KNIGHT,
}


@dataclass(slots=True)
class CompletedGame:
    game: chess.pgn.Game
    path: Path


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


def command_parts(command: str) -> list[str]:
    return shlex.split(command, posix=False)


def describe_outcome(outcome: chess.Outcome | None) -> str:
    if outcome is None:
        return "Game ended."
    if outcome.termination == chess.Termination.CHECKMATE:
        winner = "White" if outcome.winner == chess.WHITE else "Black"
        return f"Checkmate. {winner} wins."
    if outcome.termination == chess.Termination.STALEMATE:
        return "Draw by stalemate."
    if outcome.termination == chess.Termination.INSUFFICIENT_MATERIAL:
        return "Draw by insufficient material."
    if outcome.termination == chess.Termination.SEVENTYFIVE_MOVES:
        return "Draw by seventy-five move rule."
    if outcome.termination == chess.Termination.FIVEFOLD_REPETITION:
        return "Draw by fivefold repetition."
    return "Game ended."


class HumanVsEngineApp:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.events: Queue[dict[str, Any]] = Queue()
        self.engine: chess.engine.SimpleEngine | None = None
        self.engine_request_id = 0
        self.engine_thinking = False
        self.flipped = False
        self.selected_square: int | None = None
        self.legal_targets: set[int] = set()
        self.last_move: tuple[int, int] | None = None
        self.move_sans: list[str] = []
        self.finished_games: list[CompletedGame] = []
        self.human_color = chess.WHITE
        self.board = chess.Board()
        self.game = chess.pgn.Game()
        self.node: chess.pgn.GameNode = self.game

        self.root = tk.Tk()
        self.root.title("Play Against Proton Chess")
        self.root.geometry("1180x780")
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
        self.canvas.bind("<Button-1>", self.on_board_click)

        sidebar = ttk.Frame(self.root, padding=12)
        sidebar.grid(row=0, column=1, sticky="nsew")
        sidebar.columnconfigure(0, weight=1)
        sidebar.rowconfigure(3, weight=1)

        controls = ttk.Frame(sidebar)
        controls.grid(row=0, column=0, sticky="we")

        self.color_var = tk.StringVar(value=self.args.human_color.title())
        self.status_var = tk.StringVar(value="Starting engine...")
        self.title_var = tk.StringVar(value="Human vs Engine")

        ttk.Label(controls, text="Play As").grid(row=0, column=0, sticky="w")
        self.color_box = ttk.Combobox(controls, values=["White", "Black"], textvariable=self.color_var, state="readonly", width=8)
        self.color_box.grid(row=0, column=1, sticky="w", padx=(8, 16))
        ttk.Button(controls, text="New Game", command=self.start_new_game).grid(row=0, column=2, sticky="w", padx=(0, 8))
        ttk.Button(controls, text="Flip Board", command=self.flip_board).grid(row=0, column=3, sticky="w")

        ttk.Label(sidebar, textvariable=self.title_var, font=("Segoe UI", 18, "bold")).grid(
            row=1, column=0, sticky="w", pady=(12, 0)
        )
        ttk.Label(sidebar, text=f"Engine: {self.args.engine_name}", font=("Segoe UI", 11)).grid(
            row=2, column=0, sticky="w", pady=(6, 8)
        )

        self.moves_box = tk.Text(sidebar, width=44, height=28, font=("Consolas", 11), state="disabled")
        self.moves_box.grid(row=3, column=0, sticky="nsew")

        ttk.Label(sidebar, textvariable=self.status_var, wraplength=420, font=("Segoe UI", 11)).grid(
            row=4, column=0, sticky="we", pady=(10, 0)
        )

        self.connect_engine()
        self.start_new_game()

    def run(self) -> None:
        self.root.after(100, self.poll_events)
        self.root.mainloop()

    def connect_engine(self) -> None:
        try:
            self.engine = chess.engine.SimpleEngine.popen_uci(command_parts(self.args.engine))
            self.engine.configure({"Threads": 1, "Hash": 64})
            try:
                self.engine.configure({"Backend": "cpu"})
            except chess.engine.EngineError:
                pass
            if self.args.engine_option:
                self.engine.configure(dict(parse_option(option) for option in self.args.engine_option))
        except Exception as exc:
            raise RuntimeError(f"failed to start engine: {exc}") from exc

    def close(self) -> None:
        if self.engine is not None:
            try:
                self.engine.quit()
            except Exception:
                pass
        self.root.destroy()

    def poll_events(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                self.handle_event(event)
        except Empty:
            pass
        if self.root.winfo_exists():
            self.root.after(100, self.poll_events)

    def handle_event(self, event: dict[str, Any]) -> None:
        if event["kind"] == "engine_move":
            if event["request_id"] != self.engine_request_id:
                return
            self.engine_thinking = False
            move = event["move"]
            if move is None:
                self.status_var.set("Engine returned no move.")
                return
            self.apply_move(move)
        elif event["kind"] == "engine_error":
            if event["request_id"] != self.engine_request_id:
                return
            self.engine_thinking = False
            self.status_var.set(event["message"])
            messagebox.showerror("Engine error", event["message"])

    def start_new_game(self) -> None:
        if self.engine_thinking:
            messagebox.showinfo("Engine busy", "Wait for the engine to finish thinking before starting a new game.")
            return

        self.engine_request_id += 1
        self.human_color = chess.WHITE if self.color_var.get().lower() == "white" else chess.BLACK
        self.flipped = self.human_color == chess.BLACK
        self.board = chess.Board(self.args.start_fen) if self.args.start_fen else chess.Board()
        self.game = chess.pgn.Game()
        self.game.headers["Event"] = "Human vs Engine"
        self.game.headers["Date"] = datetime.now().strftime("%Y.%m.%d")
        self.game.headers["White"] = self.args.human_name if self.human_color == chess.WHITE else self.args.engine_name
        self.game.headers["Black"] = self.args.engine_name if self.human_color == chess.WHITE else self.args.human_name
        if self.args.start_fen:
            self.game.setup(self.board)
        self.node = self.game
        self.move_sans = []
        self.last_move = None
        self.clear_selection()
        self.refresh_moves()
        self.draw_board()
        side = "White" if self.human_color == chess.WHITE else "Black"
        self.title_var.set(f"{self.args.human_name} ({side}) vs {self.args.engine_name}")
        self.status_var.set("Your move." if self.board.turn == self.human_color else f"{self.args.engine_name} to move.")

        if self.board.turn != self.human_color:
            self.request_engine_move()

    def flip_board(self) -> None:
        self.flipped = not self.flipped
        self.draw_board()

    def set_moves_text(self, text: str) -> None:
        self.moves_box.configure(state="normal")
        self.moves_box.delete("1.0", tk.END)
        self.moves_box.insert("1.0", text)
        self.moves_box.see(tk.END)
        self.moves_box.configure(state="disabled")

    def refresh_moves(self) -> None:
        rows: list[str] = []
        for index in range(0, len(self.move_sans), 2):
            move_number = (index // 2) + 1
            white = self.move_sans[index]
            black = self.move_sans[index + 1] if index + 1 < len(self.move_sans) else ""
            rows.append(f"{move_number:>3}. {white:<8} {black}")
        self.set_moves_text("\n".join(rows))

    def draw_board(self) -> None:
        self.canvas.delete("all")
        for rank_index in range(8):
            for file_index in range(8):
                square = self.square_for_display(file_index, rank_index)
                x0 = BOARD_MARGIN + (file_index * SQUARE_PIXELS)
                y0 = BOARD_MARGIN + (rank_index * SQUARE_PIXELS)
                x1 = x0 + SQUARE_PIXELS
                y1 = y0 + SQUARE_PIXELS
                rank = chess.square_rank(square)
                file = chess.square_file(square)
                fill = LIGHT_SQUARE if (rank + file) % 2 == 0 else DARK_SQUARE
                if self.last_move and square in self.last_move:
                    fill = LAST_MOVE_FILL
                if square == self.selected_square:
                    fill = SELECTED_FILL
                self.canvas.create_rectangle(x0, y0, x1, y1, fill=fill, outline=fill)

                if square in self.legal_targets:
                    self.canvas.create_oval(
                        x0 + 24,
                        y0 + 24,
                        x1 - 24,
                        y1 - 24,
                        fill=TARGET_FILL,
                        outline="",
                    )

                piece = self.board.piece_at(square)
                if piece is not None:
                    self.canvas.create_text(
                        (x0 + x1) / 2,
                        (y0 + y1) / 2,
                        text=PIECE_GLYPHS[piece.symbol()],
                        font=("Segoe UI Symbol", 42),
                        fill="#111111" if piece.color == chess.BLACK else "#f9f9f9",
                    )

        for file_index in range(8):
            square = self.square_for_display(file_index, 7)
            label = chr(ord("a") + chess.square_file(square))
            x = BOARD_MARGIN + (file_index * SQUARE_PIXELS) + 8
            self.canvas.create_text(x, BOARD_MARGIN + BOARD_PIXELS + 10, text=label, anchor="sw", fill="white")
        for rank_index in range(8):
            square = self.square_for_display(0, rank_index)
            label = str(chess.square_rank(square) + 1)
            y = BOARD_MARGIN + (rank_index * SQUARE_PIXELS) + 8
            self.canvas.create_text(8, y, text=label, anchor="nw", fill="white")

    def square_for_display(self, file_index: int, rank_index: int) -> int:
        if not self.flipped:
            return chess.square(file_index, 7 - rank_index)
        return chess.square(7 - file_index, rank_index)

    def square_from_coords(self, x: int, y: int) -> int | None:
        if x < BOARD_MARGIN or y < BOARD_MARGIN:
            return None
        board_x = x - BOARD_MARGIN
        board_y = y - BOARD_MARGIN
        if board_x >= BOARD_PIXELS or board_y >= BOARD_PIXELS:
            return None
        file_index = board_x // SQUARE_PIXELS
        rank_index = board_y // SQUARE_PIXELS
        return self.square_for_display(file_index, rank_index)

    def on_board_click(self, event: tk.Event[tk.Canvas]) -> None:
        if self.engine_thinking or self.board.turn != self.human_color:
            return
        if self.board.is_game_over(claim_draw=False):
            return

        square = self.square_from_coords(event.x, event.y)
        if square is None:
            return

        piece = self.board.piece_at(square)
        if self.selected_square is None:
            if piece is not None and piece.color == self.human_color:
                self.select_square(square)
            return

        if piece is not None and piece.color == self.human_color:
            self.select_square(square)
            return

        matching_moves = [
            move
            for move in self.board.legal_moves
            if move.from_square == self.selected_square and move.to_square == square
        ]
        if not matching_moves:
            self.clear_selection()
            self.draw_board()
            return

        move = self.choose_move(matching_moves)
        if move is None:
            return

        self.apply_move(move)

    def select_square(self, square: int) -> None:
        self.selected_square = square
        self.legal_targets = {
            move.to_square for move in self.board.legal_moves if move.from_square == self.selected_square
        }
        self.draw_board()

    def clear_selection(self) -> None:
        self.selected_square = None
        self.legal_targets.clear()

    def choose_move(self, matching_moves: list[chess.Move]) -> chess.Move | None:
        if len(matching_moves) == 1:
            return matching_moves[0]

        response = simpledialog.askstring(
            "Promotion",
            "Promote to q, r, b, or n:",
            initialvalue="q",
            parent=self.root,
        )
        if response is None:
            return None
        promotion = PROMOTION_MAP.get(response.strip().lower())
        if promotion is None:
            messagebox.showinfo("Invalid promotion", "Use q, r, b, or n for promotion.")
            return None

        for move in matching_moves:
            if move.promotion == promotion:
                return move
        messagebox.showinfo("Invalid promotion", "That promotion is not legal in this position.")
        return None

    def apply_move(self, move: chess.Move) -> None:
        san = self.board.san(move)
        self.board.push(move)
        self.node = self.node.add_variation(move)
        self.move_sans.append(san)
        self.last_move = (move.from_square, move.to_square)
        self.clear_selection()
        self.refresh_moves()
        self.draw_board()

        if self.board.is_game_over(claim_draw=False):
            self.finish_game()
            return

        if self.board.turn == self.human_color:
            self.status_var.set("Your move.")
        else:
            self.request_engine_move()

    def request_engine_move(self) -> None:
        if self.engine is None:
            self.status_var.set("Engine is not running.")
            return

        self.engine_request_id += 1
        request_id = self.engine_request_id
        board_copy = self.board.copy(stack=True)
        limit = chess.engine.Limit(time=self.args.seconds)
        self.engine_thinking = True
        self.status_var.set(f"{self.args.engine_name} is thinking...")

        def worker() -> None:
            try:
                result = self.engine.play(board_copy, limit)
                self.events.put({"kind": "engine_move", "request_id": request_id, "move": result.move})
            except Exception as exc:
                self.events.put({"kind": "engine_error", "request_id": request_id, "message": str(exc)})

        threading.Thread(target=worker, daemon=True).start()

    def finish_game(self) -> None:
        outcome = self.board.outcome(claim_draw=False)
        result = self.board.result(claim_draw=False)
        self.game.headers["Result"] = result
        self.game.headers["Termination"] = describe_outcome(outcome)
        self.status_var.set(f"{self.game.headers['Termination']} Result {result}.")
        self.engine_thinking = False

        if self.args.pgn:
            self.finished_games.append(CompletedGame(self.game, self.args.pgn))
            self.save_games()

    def save_games(self) -> None:
        if not self.finished_games:
            return

        target = self.finished_games[0].path
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open("w", encoding="utf-8", newline="\n") as handle:
            for completed in self.finished_games:
                print(completed.game, file=handle, end="\n\n")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Play a local game against a UCI engine in a simple Tkinter UI.")
    parser.add_argument("engine", nargs="?", default="build\\proton_chess.exe", help="Path to the engine executable or command.")
    parser.add_argument("--seconds", type=float, default=0.2, help="Fixed think time per engine move.")
    parser.add_argument("--human-color", choices=["white", "black"], default="white", help="Color to play.")
    parser.add_argument("--human-name", default="Human", help="Display name for the human player.")
    parser.add_argument("--engine-name", help="Display name for the engine.")
    parser.add_argument("--engine-option", action="append", default=[], help="Extra UCI option, e.g. Threads=4.")
    parser.add_argument("--pgn", type=Path, help="Optional PGN output path for completed games.")
    parser.add_argument("--start-fen", help="Optional starting FEN.")
    return parser


def main() -> None:
    parser = build_parser()
    args = parser.parse_args()
    if not args.engine_name:
        args.engine_name = Path(command_parts(args.engine)[0]).stem
    app = HumanVsEngineApp(args)
    app.run()


if __name__ == "__main__":
    main()
