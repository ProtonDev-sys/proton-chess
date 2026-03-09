from __future__ import annotations

import argparse
import io
import math
import urllib.parse
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

import chess
import chess.pgn

try:
    import zstandard
except ImportError:  # pragma: no cover - optional dependency for .zst sources
    zstandard = None


@dataclass(slots=True)
class EdgeStats:
    count: int = 0
    points: float = 0.0
    child: "BookNode" = field(default_factory=lambda: BookNode())


@dataclass(slots=True)
class BookNode:
    moves: dict[str, EdgeStats] = field(default_factory=dict)


def side_to_move_points(result: str, turn: bool) -> float:
    if result == "1-0":
        return 1.0 if turn == chess.WHITE else 0.0
    if result == "0-1":
        return 1.0 if turn == chess.BLACK else 0.0
    if result == "1/2-1/2":
        return 0.5
    return -1.0


def open_pgn_text(path: Path):
    suffixes = path.suffixes
    if suffixes[-2:] == [".pgn", ".zst"] or path.suffix == ".zst":
        if zstandard is None:
            raise SystemExit("zstandard is required to read .zst PGNs. Install it with: python -m pip install zstandard")
        raw = path.open("rb")
        dctx = zstandard.ZstdDecompressor()
        reader = dctx.stream_reader(raw)
        return io.TextIOWrapper(reader, encoding="utf-8", errors="replace")
    return path.open("r", encoding="utf-8", errors="replace")


def read_int(value: str | None) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except ValueError:
        return None


def game_is_usable(game: chess.pgn.Game, min_avg_elo: int, min_plies: int, require_elo: bool) -> bool:
    result = game.headers.get("Result", "")
    if result not in {"1-0", "0-1", "1/2-1/2"}:
        return False
    if len(list(game.mainline_moves())) < min_plies:
        return False

    variant = game.headers.get("Variant", "Standard")
    if variant not in {"", "Standard"}:
        return False
    fen = game.headers.get("FEN", "")
    if fen and fen != chess.STARTING_FEN:
        return False

    average_elo = None
    white_elo = read_int(game.headers.get("WhiteElo"))
    black_elo = read_int(game.headers.get("BlackElo"))
    if white_elo is not None and black_elo is not None:
        average_elo = (white_elo + black_elo) // 2
    if require_elo and average_elo is None:
        return False
    if average_elo is not None and average_elo < min_avg_elo:
        return False

    if game.headers.get("WhiteTitle") == "BOT" or game.headers.get("BlackTitle") == "BOT":
        return False
    return True


def update_tree(root: BookNode, game: chess.pgn.Game, max_ply: int) -> None:
    board = game.board()
    result = game.headers["Result"]
    node = root

    for ply, move in enumerate(game.mainline_moves()):
        if ply >= max_ply:
            break
        move_uci = move.uci()
        edge = node.moves.setdefault(move_uci, EdgeStats())
        edge.count += 1
        points = side_to_move_points(result, board.turn)
        if points >= 0.0:
            edge.points += points
        board.push(move)
        node = edge.child


def rank_children(node: BookNode, branch_limit: int, min_count: int, min_ratio: float) -> list[tuple[str, EdgeStats]]:
    if not node.moves:
        return []

    total = sum(edge.count for edge in node.moves.values())
    ranked: list[tuple[str, EdgeStats, tuple[float, int, int]]] = []
    for move, edge in node.moves.items():
        share = edge.count / total if total else 0.0
        if edge.count < min_count or share < min_ratio:
            continue
        quality = edge.points / edge.count if edge.count else 0.0
        rank_key = (edge.count, quality, len(edge.child.moves))
        ranked.append((move, edge, rank_key))

    ranked.sort(key=lambda item: item[2], reverse=True)
    return [(move, edge) for move, edge, _ in ranked[:branch_limit]]


def build_lines(
    node: BookNode,
    prefix: list[str],
    weight: int,
    max_ply: int,
    root_branching: int,
    branch_limit: int,
    min_count: int,
    min_ratio: float,
    lines: list[tuple[int, list[str]]],
) -> None:
    if len(prefix) >= max_ply or not node.moves:
        if prefix:
            lines.append((weight, prefix.copy()))
        return

    current_limit = root_branching if not prefix else branch_limit
    children = rank_children(node, current_limit, min_count, min_ratio)
    if not children:
        if prefix:
            lines.append((weight, prefix.copy()))
        return

    for move, edge in children:
        child_weight = edge.count if not prefix else min(weight, edge.count)
        prefix.append(move)
        build_lines(
            edge.child,
            prefix,
            child_weight,
            max_ply,
            root_branching,
            branch_limit,
            min_count,
            min_ratio,
            lines,
        )
        prefix.pop()


def write_book(output: Path, lines: list[tuple[int, list[str]]]) -> None:
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8", newline="\n") as handle:
        handle.write("# Generated opening book lines.\n")
        handle.write("# Format: <weight> | <uci moves...>\n")
        for weight, moves in lines:
            handle.write(f"{weight} | {' '.join(moves)}\n")


def download_inputs(urls: list[str], cache_dir: Path) -> list[Path]:
    cache_dir.mkdir(parents=True, exist_ok=True)
    downloaded: list[Path] = []
    for url in urls:
        target = cache_dir / Path(urllib.parse.urlparse(url).path).name
        if not target.exists():
            print(f"downloading {url} -> {target}")
            urllib.request.urlretrieve(url, target)
        else:
            print(f"using cached {target}")
        downloaded.append(target)
    return downloaded


def main() -> None:
    parser = argparse.ArgumentParser(description="Build a weighted opening book from PGN or PGN.zst sources.")
    parser.add_argument("inputs", nargs="*", type=Path, help="Local PGN or PGN.zst inputs.")
    parser.add_argument("--download-url", action="append", default=[], help="Download a PGN source into the cache before building.")
    parser.add_argument("--cache-dir", type=Path, default=Path("openings/data"))
    parser.add_argument("--output", type=Path, default=Path("openings/book_lines.txt"))
    parser.add_argument("--max-games", type=int, default=25000)
    parser.add_argument("--max-ply", type=int, default=16)
    parser.add_argument("--min-plies", type=int, default=8)
    parser.add_argument("--min-avg-elo", type=int, default=2400)
    parser.add_argument("--require-elo", action="store_true")
    parser.add_argument("--root-branching", type=int, default=10)
    parser.add_argument("--branching", type=int, default=4)
    parser.add_argument("--min-count", type=int, default=8)
    parser.add_argument("--min-ratio", type=float, default=0.04)
    args = parser.parse_args()

    inputs = list(args.inputs)
    if args.download_url:
        inputs.extend(download_inputs(args.download_url, args.cache_dir))
    if not inputs:
        raise SystemExit("Provide at least one PGN input or one --download-url.")

    root = BookNode()
    seen = 0
    used = 0

    for path in inputs:
        print(f"reading {path}")
        with open_pgn_text(path) as handle:
            while used < args.max_games:
                game = chess.pgn.read_game(handle)
                if game is None:
                    break
                seen += 1
                if not game_is_usable(game, args.min_avg_elo, args.min_plies, args.require_elo):
                    continue
                update_tree(root, game, args.max_ply)
                used += 1
                if used % 1000 == 0:
                    print(f"accepted {used} / seen {seen}")
        if used >= args.max_games:
            break

    if used == 0:
        raise SystemExit("No usable games matched the filters.")

    lines: list[tuple[int, list[str]]] = []
    build_lines(
        root,
        [],
        0,
        args.max_ply,
        args.root_branching,
        args.branching,
        args.min_count,
        args.min_ratio,
        lines,
    )
    lines.sort(key=lambda item: (item[0], len(item[1])), reverse=True)
    write_book(args.output, lines)
    print(f"wrote {len(lines)} lines from {used} accepted games (seen {seen}) -> {args.output}")


if __name__ == "__main__":
    main()
