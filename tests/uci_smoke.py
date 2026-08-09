#!/usr/bin/env python3
"""Minimal asynchronous UCI protocol regression test for Proton Chess."""

from __future__ import annotations

import queue
import subprocess
import sys
import threading
import time
from pathlib import Path


class EngineProcess:
    def __init__(self, binary: Path) -> None:
        self.process = subprocess.Popen(
            [str(binary)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert self.process.stdin is not None
        assert self.process.stdout is not None
        self.lines: queue.Queue[str] = queue.Queue()
        self.reader = threading.Thread(target=self._read_stdout, daemon=True)
        self.reader.start()

    def _read_stdout(self) -> None:
        assert self.process.stdout is not None
        for line in self.process.stdout:
            self.lines.put(line.rstrip("\r\n"))

    def send(self, command: str) -> None:
        assert self.process.stdin is not None
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def wait_for(self, predicate, timeout: float = 5.0) -> tuple[str, list[str]]:
        deadline = time.monotonic() + timeout
        seen: list[str] = []
        while time.monotonic() < deadline:
            remaining = max(0.01, deadline - time.monotonic())
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                break
            seen.append(line)
            if predicate(line):
                return line, seen
        raise AssertionError(f"timed out; engine output was: {seen!r}")

    def assert_no_match(self, predicate, duration: float = 0.25) -> None:
        deadline = time.monotonic() + duration
        seen: list[str] = []
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=max(0.01, deadline - time.monotonic()))
            except queue.Empty:
                break
            seen.append(line)
            if predicate(line):
                raise AssertionError(f"unexpected engine output: {line!r}; seen: {seen!r}")

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=3)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()
                self.process.wait(timeout=3)
        stderr = ""
        if self.process.stderr is not None:
            stderr = self.process.stderr.read()
        if self.process.returncode not in (0, None):
            raise AssertionError(f"engine exited with {self.process.returncode}: {stderr}")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: uci_smoke.py /path/to/proton_chess", file=sys.stderr)
        return 2

    binary = Path(sys.argv[1]).resolve()
    if not binary.is_file():
        raise FileNotFoundError(binary)

    engine = EngineProcess(binary)
    try:
        engine.send("uci")
        _, uci_lines = engine.wait_for(lambda line: line == "uciok")
        assert any("option name Threads type spin default 1 min 1 max 1" in line
                   for line in uci_lines)
        assert any("option name BookRandomness type spin default 0 min 0 max 100" in line
                   for line in uci_lines)
        assert not any("option name Backend" in line for line in uci_lines)
        assert not any("option name SyzygyPath" in line for line in uci_lines)

        engine.send("isready")
        engine.wait_for(lambda line: line == "readyok")

        engine.send("position startpos moves e2e4 e7e5")
        engine.send("d")
        fen_line, _ = engine.wait_for(lambda line: line.startswith("info string fen "))
        expected = "info string fen rnbqkbnr/pppp1ppp/8/4p3/4P3/8/PPPP1PPP/RNBQKBNR w KQkq e6 0 2"
        assert fen_line == expected, fen_line

        # A malformed command must not leave a partially updated position behind.
        engine.send("position startpos moves e2e5")
        engine.wait_for(lambda line: "illegal move in position command" in line)
        engine.send("d")
        after_invalid, _ = engine.wait_for(lambda line: line.startswith("info string fen "))
        assert after_invalid == expected, after_invalid

        engine.send("position startpos")
        engine.send("moves")
        engine.send("isready")
        _, move_lines = engine.wait_for(lambda line: line == "readyok")
        legal_moves = sorted(line for line in move_lines if len(line) in (4, 5))
        assert legal_moves == [
            "a2a3", "a2a4", "b1a3", "b1c3", "b2b3", "b2b4", "c2c3", "c2c4",
            "d2d3", "d2d4", "e2e3", "e2e4", "f2f3", "f2f4", "g1f3", "g1h3",
            "g2g3", "g2g4", "h2h3", "h2h4",
        ], legal_moves

        engine.send("perft 2")
        perft_line, _ = engine.wait_for(lambda line: line.startswith("info string perft "))
        assert "depth 2 nodes 400 " in perft_line, perft_line

        engine.send("position startpos")
        engine.send("go depth 4 searchmoves e2e4")
        bestmove, _ = engine.wait_for(lambda line: line.startswith("bestmove "), timeout=10)
        assert bestmove.split()[1] == "e2e4", bestmove

        # If a node limit interrupts the next iteration, bestmove must still
        # match the principal variation from the last fully completed depth.
        engine.send("ucinewgame")
        engine.send("position startpos")
        engine.send("go nodes 25000")
        limited_best, limited_lines = engine.wait_for(
            lambda line: line.startswith("bestmove "), timeout=10)
        completed_infos = [line for line in limited_lines
                           if line.startswith("info depth ") and " pv " in line]
        assert completed_infos, limited_lines
        last_pv = completed_infos[-1].split(" pv ", 1)[1].split()[0]
        assert limited_best.split()[1] == last_pv, (completed_infos[-1], limited_best)

        engine.send("position startpos")
        engine.send("go infinite")
        engine.wait_for(lambda line: line.startswith("info depth "), timeout=5)
        engine.send("stop")
        stopped, _ = engine.wait_for(lambda line: line.startswith("bestmove "), timeout=5)
        assert stopped.split()[1] != "0000", stopped

        # A completed ponder search must withhold bestmove until ponderhit, and
        # ponderhit must release/continue it rather than acting like stop.
        engine.send("position startpos")
        engine.send("go ponder depth 1")
        engine.wait_for(lambda line: line.startswith("info depth 1 "), timeout=5)
        engine.assert_no_match(lambda line: line.startswith("bestmove "))
        engine.send("ponderhit")
        pondered, _ = engine.wait_for(lambda line: line.startswith("bestmove "), timeout=5)
        assert pondered.split()[1] != "0000", pondered
    finally:
        engine.close()

    print("UCI smoke tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
