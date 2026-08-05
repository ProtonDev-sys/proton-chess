#!/usr/bin/env python3
"""Run a small deterministic UCI search benchmark without external packages."""

from __future__ import annotations

import argparse
import json
import subprocess
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
    parser.add_argument("--json", action="store_true", dest="as_json")
    return parser.parse_args()


def send(process: subprocess.Popen[str], command: str) -> None:
    assert process.stdin is not None
    process.stdin.write(command + "\n")
    process.stdin.flush()


def read_until(process: subprocess.Popen[str], prefix: str, timeout: float = 30.0) -> tuple[str, list[str]]:
    assert process.stdout is not None
    deadline = time.monotonic() + timeout
    lines: list[str] = []
    while time.monotonic() < deadline:
        line = process.stdout.readline()
        if not line:
            if process.poll() is not None:
                raise RuntimeError(f"engine exited with code {process.returncode}")
            continue
        line = line.rstrip("\r\n")
        lines.append(line)
        if line.startswith(prefix):
            return line, lines
    raise TimeoutError(f"timed out waiting for {prefix!r}; output={lines!r}")


def parse_info(lines: list[str], depth: int) -> dict[str, object]:
    selected = ""
    for line in lines:
        if line.startswith(f"info depth {depth} "):
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

    process = subprocess.Popen(
        [str(binary)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    results: list[dict[str, object]] = []
    try:
        send(process, "uci")
        read_until(process, "uciok")
        send(process, f"setoption name Hash value {max(1, args.hash)}")
        send(process, "setoption name UseBook value false")
        send(process, "setoption name HumanStyle value false")
        send(process, "isready")
        read_until(process, "readyok")

        for name, position_command in POSITIONS:
            send(process, "ucinewgame")
            send(process, f"position {position_command}")
            send(process, f"go depth {args.depth}")
            bestmove, lines = read_until(process, "bestmove ")
            row = {"position": name, "bestmove": bestmove.split()[1]}
            row.update(parse_info(lines, args.depth))
            results.append(row)
    finally:
        if process.poll() is None:
            try:
                send(process, "quit")
                process.wait(timeout=3)
            except Exception:
                process.kill()
                process.wait(timeout=3)

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
