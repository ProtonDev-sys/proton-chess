#!/usr/bin/env python3
"""Run a small deterministic UCI search benchmark without external packages."""

from __future__ import annotations

import argparse
import json
import queue
import subprocess
import threading
import time
from pathlib import Path

POSITIONS = [
    ("startpos", "startpos"),
    ("kiwipete", "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"),
    ("endgame", "fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"),
]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=Path)
    parser.add_argument("--depth", type=int, default=10)
    parser.add_argument("--hash", type=int, default=64)
    parser.add_argument("--nodes", type=int, help="fixed node budget instead of fixed depth")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def send(process: subprocess.Popen[str], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write(command + "\n")
    process.stdin.flush()


def read_until(process: subprocess.Popen[str], output: queue.Queue[str | None],
               prefix: str, timeout: float = 30.0) -> tuple[str, list[str]]:
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    while time.monotonic() < deadline:
        try:
            line = output.get(timeout=max(0.001, deadline - time.monotonic()))
        except queue.Empty:
            break
        if line is None:
            raise RuntimeError(f"engine closed stdout; exit code={process.poll()}")
        line = line.rstrip("\r\n")
        lines.append(line)
        if line.startswith(prefix):
            return line, lines
    raise TimeoutError(f"timed out waiting for {prefix!r}; output={lines!r}")


def parse_info(lines: list[str], depth: int | None) -> dict[str, object]:
    selected = ""
    for line in lines:
        if line.startswith("info depth ") and (depth is None or line.startswith(f"info depth {depth} ")):
            selected = line
    result: dict[str, object] = {"info": selected}
    if not selected:
        return result
    tokens = selected.split()
    for key in ("depth", "seldepth", "nodes", "nps", "hashfull", "time"):
        if key in tokens:
            result[key] = int(tokens[tokens.index(key) + 1])
    if "score" in tokens:
        index = tokens.index("score")
        result["score_type"] = tokens[index + 1]
        result["score"] = int(tokens[index + 2])
    if "pv" in tokens:
        result["pv"] = tokens[tokens.index("pv") + 1 :]
    return result


def main() -> int:
    args = parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)
    if args.depth < 1 or args.depth > 126:
        raise ValueError("depth must be between 1 and 126")
    if args.nodes is not None and args.nodes < 1:
        raise ValueError("nodes must be positive")
    if args.timeout <= 0:
        raise ValueError("timeout must be positive")

    process = subprocess.Popen(
        [str(binary)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
    )
    output: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            output.put(line)
        output.put(None)

    reader = threading.Thread(target=read_output, daemon=True)
    reader.start()
    results: list[dict[str, object]] = []
    try:
        send(process, "uci")
        read_until(process, output, "uciok", args.timeout)
        send(process, f"setoption name Hash value {max(1, args.hash)}")
        send(process, "setoption name UseBook value false")
        send(process, "setoption name HumanStyle value false")
        send(process, "isready")
        read_until(process, output, "readyok", args.timeout)

        for name, position_command in POSITIONS:
            send(process, "ucinewgame")
            send(process, f"position {position_command}")
            send(process, f"go nodes {args.nodes}" if args.nodes else f"go depth {args.depth}")
            bestmove, lines = read_until(process, output, "bestmove ", args.timeout)
            row = {"position": name, "bestmove": bestmove.split()[1]}
            row.update(parse_info(lines, None if args.nodes else args.depth))
            results.append(row)
    finally:
        if process.poll() is None:
            try:
                send(process, "quit")
                process.wait(timeout=3)
            except Exception:
                process.kill()
                process.wait(timeout=3)
        reader.join(timeout=3)

    if args.as_json:
        print(json.dumps(results, indent=2))
    else:
        for row in results:
            print(
                f"{row['position']:10} best={row['bestmove']:5} "
                f"nodes={row.get('nodes', '?')} time_ms={row.get('time', '?')} "
                f"nps={row.get('nps', '?')} score={row.get('score_type', '?')}:{row.get('score', '?')}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
