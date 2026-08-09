#!/usr/bin/env python3
"""Compare two Proton binaries over the same deterministic UCI searches."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import platform
import queue
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import threading
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable

import chess


ROOT = Path(__file__).resolve().parents[1]
OPTION_ORDER = (
    "Hash",
    "Threads",
    "UseBook",
    "BookRandomness",
    "Skill Level",
    "HumanStyle",
    "HumanSkill",
    "HumanMaxLossCp",
    "HumanSeed",
    "MoveOverhead",
    "Contempt",
    "UCI_LimitStrength",
)
OPTION_PATTERN = re.compile(r"^option name (.+?) type ")
UCI_MOVE_PATTERN = re.compile(r"^[a-h][1-8][a-h][1-8][qrbn]?$")
SEMANTIC_FIELDS = (
    "bestmove",
    "ponder",
    "score",
    "depth",
    "seldepth",
    "nodes",
    "pv",
)
U64_MAX = 2**64 - 1
MAX_TIMEOUT_SECONDS = 24.0 * 60.0 * 60.0


class EngineFailure(RuntimeError):
    """A UCI process exited or produced an unusable search result."""


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Run paired deterministic searches with a candidate and baseline Proton "
            "binary, alternating which engine searches first."
        )
    )
    parser.add_argument("candidate", type=Path)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("--candidate-label", default="candidate")
    parser.add_argument("--baseline-label", default="baseline")
    parser.add_argument(
        "--openings",
        type=Path,
        default=Path("openings/uho_lichess_4852_v1_200.epd"),
        help="six-field FEN/EPD file in deterministic comparison order",
    )
    parser.add_argument(
        "--positions",
        type=int,
        help="compare only the first N non-comment positions (default: all)",
    )
    limit = parser.add_mutually_exclusive_group()
    limit.add_argument("--depth", type=int, help="fixed search depth (default: 10)")
    limit.add_argument("--nodes", type=int, help="exact UCI node limit")
    parser.add_argument("--hash", type=int, default=16, dest="hash_mb")
    parser.add_argument(
        "--trials",
        type=int,
        default=2,
        help="fresh-process repetitions; execution order flips each trial",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=60.0,
        help="hard wall-clock deadline for each UCI response/search",
    )
    parser.add_argument(
        "--json",
        type=Path,
        required=True,
        dest="json_path",
        help="atomic checkpoint and final report path",
    )
    args = parser.parse_args(argv)
    if args.depth is None and args.nodes is None:
        args.depth = 10
    validate_args(args)
    return args


def validate_args(args: argparse.Namespace) -> None:
    if args.depth is not None and not 1 <= args.depth <= 126:
        raise ValueError("depth must be between 1 and 126")
    if args.nodes is not None and not 1 <= args.nodes <= U64_MAX:
        raise ValueError("nodes must be between 1 and 2^64-1")
    if args.hash_mb < 1 or args.hash_mb > 4096:
        raise ValueError("hash must be between 1 and 4096 MB")
    if args.positions is not None and args.positions < 1:
        raise ValueError("positions must be positive")
    if args.trials < 2:
        raise ValueError("trials must be at least two for a repeatability check")
    if (
        not math.isfinite(args.timeout)
        or args.timeout <= 0.0
        or args.timeout > MAX_TIMEOUT_SECONDS
    ):
        raise ValueError("timeout must be positive and no more than 86400 seconds")
    if not args.candidate_label.strip() or not args.baseline_label.strip():
        raise ValueError("engine labels must not be empty")
    if args.candidate_label == args.baseline_label:
        raise ValueError("candidate and baseline labels must be different")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json_atomic(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            newline="\n",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as handle:
            temporary = Path(handle.name)
            json.dump(payload, handle, indent=2)
            handle.write("\n")
        temporary.replace(path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def stage_file(source: Path, destination: Path) -> str:
    before = sha256_file(source)
    shutil.copy2(source, destination)
    staged = sha256_file(destination)
    after = sha256_file(source)
    if before != staged or after != staged:
        destination.unlink(missing_ok=True)
        raise ValueError(f"input changed while it was staged: {source}")
    return staged


def git_revision(root: Path) -> str | None:
    try:
        result = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    revision = result.stdout.strip()
    return revision if result.returncode == 0 and revision else None


def git_dirty(root: Path) -> bool | None:
    try:
        result = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=root,
            text=True,
            capture_output=True,
            check=False,
            timeout=5,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return None
    return bool(result.stdout.strip()) if result.returncode == 0 else None


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def load_positions(path: Path, count: int | None = None) -> list[dict[str, Any]]:
    positions: list[dict[str, Any]] = []
    seen: set[str] = set()
    with path.open(encoding="utf-8") as handle:
        for line_number, raw_line in enumerate(handle, start=1):
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) < 6:
                raise ValueError(f"line {line_number} is not a six-field FEN")
            fen = " ".join(fields[:6])
            fen = validate_fen(fen, line_number)
            if fen in seen:
                raise ValueError(f"duplicate FEN on line {line_number}: {fen}")
            seen.add(fen)
            positions.append(
                {"index": len(positions) + 1, "line_number": line_number, "fen": fen}
            )
            if count is not None and len(positions) == count:
                break
    if not positions:
        raise ValueError(f"no positions found in {path}")
    if count is not None and len(positions) != count:
        raise ValueError(
            f"requested {count} positions but {path} contains only {len(positions)}"
        )
    return positions


def validate_fen(fen: str, line_number: int) -> str:
    board, side, castling, en_passant, halfmove, fullmove = fen.split()
    ranks = board.split("/")
    if len(ranks) != 8:
        raise ValueError(f"line {line_number} has an invalid FEN board")
    for rank in ranks:
        squares = sum(int(token) if token.isdigit() else 1 for token in rank)
        if squares != 8 or any(
            not (token.isdigit() or token in "pnbrqkPNBRQK") for token in rank
        ):
            raise ValueError(f"line {line_number} has an invalid FEN rank")
    if side not in {"w", "b"}:
        raise ValueError(f"line {line_number} has an invalid side to move")
    if castling != "-" and any(token not in "KQkq" for token in castling):
        raise ValueError(f"line {line_number} has invalid castling rights")
    if en_passant != "-" and not re.fullmatch(r"[a-h][36]", en_passant):
        raise ValueError(f"line {line_number} has an invalid en-passant square")
    try:
        halfmove_value = int(halfmove)
        fullmove_value = int(fullmove)
    except ValueError as error:
        raise ValueError(f"line {line_number} has invalid FEN counters") from error
    if halfmove_value < 0 or fullmove_value < 1:
        raise ValueError(f"line {line_number} has invalid FEN counters")
    try:
        position = chess.Board(fen)
    except ValueError as error:
        raise ValueError(f"line {line_number} is not a legal FEN") from error
    if not position.is_valid():
        raise ValueError(f"line {line_number} is not a legal FEN")
    if position.is_game_over(claim_draw=True):
        raise ValueError(f"line {line_number} is already terminal")
    return position.fen(en_passant="fen")


def engine_configuration(hash_mb: int) -> dict[str, str | int | bool]:
    return {
        "Hash": hash_mb,
        "Threads": 1,
        "UseBook": False,
        "BookRandomness": 0,
        "Skill Level": 20,
        "HumanStyle": False,
        "HumanSkill": 20,
        "HumanMaxLossCp": 12,
        "HumanSeed": "1",
        "MoveOverhead": 25,
        "Contempt": 0,
        "UCI_LimitStrength": False,
    }


def uci_value(value: str | int | bool) -> str:
    if isinstance(value, bool):
        return "true" if value else "false"
    return str(value)


class EngineProcess:
    """A line-oriented UCI process with queue-backed, non-blocking deadlines."""

    def __init__(self, path: Path, label: str, timeout: float) -> None:
        self.path = path
        self.label = label
        self.timeout = timeout
        self._lines: queue.Queue[str | None] = queue.Queue()
        self._tail: list[str] = []
        creation_flags = 0
        if os.name == "nt":
            creation_flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.process = subprocess.Popen(
            [str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            errors="replace",
            bufsize=1,
            creationflags=creation_flags,
        )
        self._reader = threading.Thread(
            target=self._read_output,
            name=f"uci-reader-{label}",
            daemon=True,
        )
        self._reader.start()
        self.identity: dict[str, str] = {}
        self.options: set[str] = set()

    def _read_output(self) -> None:
        assert self.process.stdout is not None
        try:
            for raw_line in self.process.stdout:
                self._lines.put(raw_line.rstrip("\r\n"))
        finally:
            self._lines.put(None)

    def send(self, command: str) -> None:
        if self.process.poll() is not None:
            raise EngineFailure(
                f"{self.label} exited with code {self.process.returncode} before {command!r}"
            )
        assert self.process.stdin is not None
        try:
            self.process.stdin.write(command + "\n")
            self.process.stdin.flush()
        except (BrokenPipeError, OSError) as error:
            raise EngineFailure(f"failed to send {command!r} to {self.label}") from error

    def read_until(
        self,
        predicate: Callable[[str], bool],
        description: str,
        timeout: float | None = None,
    ) -> tuple[str, list[str]]:
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        collected: list[str] = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0.0:
                raise TimeoutError(
                    f"{self.label} timed out waiting for {description}; "
                    f"last output={self._tail[-20:]!r}"
                )
            try:
                line = self._lines.get(timeout=remaining)
            except queue.Empty as error:
                raise TimeoutError(
                    f"{self.label} timed out waiting for {description}; "
                    f"last output={self._tail[-20:]!r}"
                ) from error
            if line is None:
                raise EngineFailure(
                    f"{self.label} exited with code {self.process.poll()} while waiting "
                    f"for {description}; last output={self._tail[-20:]!r}"
                )
            collected.append(line)
            self._tail.append(line)
            del self._tail[:-200]
            if predicate(line):
                return line, collected

    def initialize(self, configuration: dict[str, str | int | bool]) -> None:
        self.send("uci")
        _, lines = self.read_until(lambda line: line == "uciok", "uciok")
        for line in lines:
            if line.startswith("id name "):
                self.identity["name"] = line.removeprefix("id name ")
            elif line.startswith("id author "):
                self.identity["author"] = line.removeprefix("id author ")
            else:
                match = OPTION_PATTERN.match(line)
                if match:
                    self.options.add(match.group(1))
        missing = [name for name in OPTION_ORDER if name not in self.options]
        if missing:
            raise EngineFailure(
                f"{self.label} does not advertise required Proton options: "
                + ", ".join(missing)
            )
        for name in OPTION_ORDER:
            self.send(f"setoption name {name} value {uci_value(configuration[name])}")
        self.synchronize()

    def synchronize(self) -> None:
        self.send("isready")
        self.read_until(lambda line: line == "readyok", "readyok")

    def set_position(self, fen: str) -> None:
        self.send(f"position fen {fen}")
        self.send("d")
        response, _ = self.read_until(
            lambda line: line.startswith("info string fen "), "position FEN echo"
        )
        actual = response.removeprefix("info string fen ")
        if actual != fen:
            raise EngineFailure(
                f"{self.label} loaded a different position: expected {fen!r}, "
                f"got {actual!r}"
            )

    def search(
        self, fen: str, go_command: str, limit: dict[str, Any]
    ) -> dict[str, Any]:
        self.send("ucinewgame")
        self.synchronize()
        self.set_position(fen)
        started = time.perf_counter()
        self.send(go_command)
        bestmove_line, lines = self.read_until(
            lambda line: line.startswith("bestmove "), "bestmove"
        )
        wall_time_ms = round((time.perf_counter() - started) * 1000.0, 3)
        result = parse_search_result(bestmove_line, lines, wall_time_ms)
        validate_search_result(fen, result, limit)
        return result

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self.send("quit")
                self.process.wait(timeout=2)
            except Exception:
                self.process.kill()
                self.process.wait(timeout=2)
        self._reader.join(timeout=1)
        for stream in (self.process.stdin, self.process.stdout):
            if stream is not None:
                stream.close()


def close_engines(engines: Iterable[EngineProcess | None]) -> list[BaseException]:
    errors: list[BaseException] = []
    for engine in engines:
        if engine is None:
            continue
        try:
            engine.close()
        except BaseException as error:
            errors.append(error)
    return errors


def parse_search_result(
    bestmove_line: str, lines: Iterable[str], wall_time_ms: float
) -> dict[str, Any]:
    bestmove_tokens = bestmove_line.split()
    valid_shape = (
        len(bestmove_tokens) == 2
        or (
            len(bestmove_tokens) == 4
            and bestmove_tokens[2] == "ponder"
            and UCI_MOVE_PATTERN.fullmatch(bestmove_tokens[3]) is not None
        )
    )
    if (
        not valid_shape
        or bestmove_tokens[0] != "bestmove"
        or not UCI_MOVE_PATTERN.fullmatch(bestmove_tokens[1])
    ):
        raise EngineFailure(f"invalid bestmove line: {bestmove_line!r}")
    info_lines = [
        line for line in lines if line.startswith("info depth ") and " pv " in line
    ]
    if not info_lines:
        raise EngineFailure(
            f"search returned {bestmove_line!r} without a completed principal variation"
        )
    selected = info_lines[-1]
    tokens = selected.split()
    result: dict[str, Any] = {
        "bestmove": bestmove_tokens[1],
        "ponder": (
            bestmove_tokens[3]
            if len(bestmove_tokens) >= 4 and bestmove_tokens[2] == "ponder"
            else None
        ),
        "wall_time_ms": wall_time_ms,
        "raw_info": selected,
    }
    pv_index = tokens.index("pv")
    header = tokens[:pv_index]
    for key in ("depth", "seldepth", "nodes", "nps", "hashfull", "time"):
        if header.count(key) > 1:
            raise EngineFailure(f"duplicate {key} field in {selected!r}")
        if key in header:
            index = header.index(key)
            try:
                result[key] = int(header[index + 1])
            except (IndexError, ValueError) as error:
                raise EngineFailure(f"invalid {key} field in {selected!r}") from error
    if header.count("score") != 1:
        raise EngineFailure(f"missing score in {selected!r}")
    score_index = header.index("score")
    try:
        score_type = header[score_index + 1]
        score_value = int(header[score_index + 2])
    except (IndexError, ValueError) as error:
        raise EngineFailure(f"invalid score in {selected!r}") from error
    if score_type not in {"cp", "mate"}:
        raise EngineFailure(f"unsupported score type in {selected!r}")
    bounds = [bound for bound in ("lowerbound", "upperbound") if bound in header]
    if len(bounds) > 1:
        raise EngineFailure(f"contradictory score bounds in {selected!r}")
    result["score"] = {
        "type": score_type,
        "value": score_value,
        "bound": bounds[0] if bounds else "exact",
    }
    result["pv"] = tokens[pv_index + 1 :]
    required = ("depth", "seldepth", "nodes", "time")
    missing = [key for key in required if key not in result]
    if missing:
        raise EngineFailure(
            f"completed search info is missing {', '.join(missing)}: {selected!r}"
        )
    if any(result[key] < 0 for key in ("depth", "seldepth", "nodes", "time")):
        raise EngineFailure(f"negative search metric in {selected!r}")
    if result["depth"] < 1 or result["seldepth"] < result["depth"]:
        raise EngineFailure(f"invalid depth metrics in {selected!r}")
    if "nps" in result and result["nps"] < 0:
        raise EngineFailure(f"negative nps in {selected!r}")
    if "hashfull" in result and not 0 <= result["hashfull"] <= 1000:
        raise EngineFailure(f"invalid hashfull in {selected!r}")
    return result


def validate_search_result(
    fen: str, result: dict[str, Any], limit: dict[str, Any]
) -> None:
    if limit["kind"] == "depth" and result["depth"] != limit["value"]:
        raise EngineFailure(
            f"depth search requested {limit['value']} but completed {result['depth']}"
        )
    if limit["kind"] == "nodes" and result["nodes"] > limit["value"]:
        raise EngineFailure(
            f"node-limited search reported {result['nodes']} nodes above the "
            f"{limit['value']} cap"
        )
    pv = result["pv"]
    if not pv or pv[0] != result["bestmove"]:
        raise EngineFailure("principal variation does not begin with bestmove")
    position = chess.Board(fen)
    for index, move_text in enumerate(pv):
        try:
            move = chess.Move.from_uci(move_text)
        except ValueError as error:
            raise EngineFailure(f"invalid PV move {move_text!r}") from error
        if move not in position.legal_moves:
            raise EngineFailure(
                f"illegal PV move {move_text!r} at ply {index + 1} from {fen}"
            )
        position.push(move)
    ponder = result.get("ponder")
    if ponder is not None:
        root = chess.Board(fen)
        root.push_uci(result["bestmove"])
        ponder_move = chess.Move.from_uci(ponder)
        if ponder_move not in root.legal_moves:
            raise EngineFailure(f"illegal ponder move {ponder!r} from {fen}")
        if len(pv) > 1 and ponder != pv[1]:
            raise EngineFailure("ponder move does not match the principal variation")


def ratio_delta(candidate: float, baseline: float) -> float | None:
    if baseline == 0:
        return None
    return (candidate / baseline - 1.0) * 100.0


def mismatch_fields(left: dict[str, Any], right: dict[str, Any]) -> list[str]:
    return [field for field in SEMANTIC_FIELDS if left[field] != right[field]]


def repeatability(rows: list[dict[str, Any]], engine: str) -> dict[str, Any]:
    grouped: dict[int, list[dict[str, Any]]] = {}
    for row in rows:
        grouped.setdefault(row["position"], []).append(row)
    mismatches: list[dict[str, Any]] = []
    eligible = 0
    for position, position_rows in sorted(grouped.items()):
        if len(position_rows) < 2:
            continue
        eligible += 1
        ordered = sorted(position_rows, key=lambda row: row["trial"])
        reference = ordered[0][engine]
        fields = sorted(
            {
                field
                for row in ordered[1:]
                for field in mismatch_fields(reference, row[engine])
            }
        )
        if fields:
            mismatches.append({"position": position, "fields": fields})
    return {
        "eligible_positions": eligible,
        "repeatable_positions": eligible - len(mismatches),
        "mismatches": mismatches,
    }


def summarize(
    rows: list[dict[str, Any]], limit_kind: str = "depth"
) -> dict[str, Any]:
    if not rows:
        return {
            "search_pairs_compared": 0,
            "bestmove_matches": 0,
            "score_matches": 0,
            "depth_matches": 0,
            "seldepth_matches": 0,
            "node_matches": 0,
            "pv_matches": 0,
            "all_semantic_matches": 0,
            "semantic_mismatches": [],
            "node_efficiency_comparable": limit_kind == "depth",
            "deterministic_across_trials": True,
        }
    candidate = [row["candidate"] for row in rows]
    baseline = [row["baseline"] for row in rows]
    candidate_nodes = sum(result["nodes"] for result in candidate)
    baseline_nodes = sum(result["nodes"] for result in baseline)
    candidate_time = sum(result["time"] for result in candidate)
    baseline_time = sum(result["time"] for result in baseline)
    candidate_wall = sum(result["wall_time_ms"] for result in candidate)
    baseline_wall = sum(result["wall_time_ms"] for result in baseline)
    semantic_mismatches = [
        {
            "trial": row["trial"],
            "position": row["position"],
            "fields": fields,
        }
        for row in rows
        if (fields := mismatch_fields(row["candidate"], row["baseline"]))
    ]
    trial_totals: list[dict[str, Any]] = []
    for trial in sorted({row["trial"] for row in rows}):
        trial_rows = [row for row in rows if row["trial"] == trial]
        trial_totals.append(
            {
                "trial": trial,
                "search_pairs": len(trial_rows),
                "candidate_info_nodes": sum(
                    row["candidate"]["nodes"] for row in trial_rows
                ),
                "baseline_info_nodes": sum(
                    row["baseline"]["nodes"] for row in trial_rows
                ),
                "candidate_engine_time_ms": sum(
                    row["candidate"]["time"] for row in trial_rows
                ),
                "baseline_engine_time_ms": sum(
                    row["baseline"]["time"] for row in trial_rows
                ),
                "candidate_wall_time_ms": round(
                    sum(row["candidate"]["wall_time_ms"] for row in trial_rows), 3
                ),
                "baseline_wall_time_ms": round(
                    sum(row["baseline"]["wall_time_ms"] for row in trial_rows), 3
                ),
            }
        )
    candidate_repeatability = repeatability(rows, "candidate")
    baseline_repeatability = repeatability(rows, "baseline")
    summary: dict[str, Any] = {
        "search_pairs_compared": len(rows),
        "unique_positions_compared": len({row["position"] for row in rows}),
        "trials_observed": len({row["trial"] for row in rows}),
        "bestmove_matches": sum(
            left["bestmove"] == right["bestmove"]
            for left, right in zip(candidate, baseline)
        ),
        "score_matches": sum(
            left["score"] == right["score"]
            for left, right in zip(candidate, baseline)
        ),
        "depth_matches": sum(
            left["depth"] == right["depth"]
            for left, right in zip(candidate, baseline)
        ),
        "seldepth_matches": sum(
            left["seldepth"] == right["seldepth"]
            for left, right in zip(candidate, baseline)
        ),
        "node_matches": sum(
            left["nodes"] == right["nodes"]
            for left, right in zip(candidate, baseline)
        ),
        "pv_matches": sum(
            left["pv"] == right["pv"] for left, right in zip(candidate, baseline)
        ),
        "all_semantic_matches": len(rows) - len(semantic_mismatches),
        "semantic_mismatches": semantic_mismatches,
        "candidate_info_nodes": candidate_nodes,
        "baseline_info_nodes": baseline_nodes,
        "node_efficiency_comparable": limit_kind == "depth",
        "candidate_engine_time_ms": candidate_time,
        "baseline_engine_time_ms": baseline_time,
        "engine_time_delta_percent": ratio_delta(candidate_time, baseline_time),
        "candidate_median_engine_time_ms": statistics.median(
            result["time"] for result in candidate
        ),
        "baseline_median_engine_time_ms": statistics.median(
            result["time"] for result in baseline
        ),
        "candidate_wall_time_ms": round(candidate_wall, 3),
        "baseline_wall_time_ms": round(baseline_wall, 3),
        "wall_time_delta_percent": ratio_delta(candidate_wall, baseline_wall),
        "trial_totals": trial_totals,
        "candidate_median_trial_engine_time_ms": statistics.median(
            trial["candidate_engine_time_ms"] for trial in trial_totals
        ),
        "baseline_median_trial_engine_time_ms": statistics.median(
            trial["baseline_engine_time_ms"] for trial in trial_totals
        ),
        "candidate_repeatability": candidate_repeatability,
        "baseline_repeatability": baseline_repeatability,
        "deterministic_across_trials": (
            not candidate_repeatability["mismatches"]
            and not baseline_repeatability["mismatches"]
        ),
    }
    if limit_kind == "depth":
        node_deltas = [
            left["nodes"] - right["nodes"]
            for left, right in zip(candidate, baseline)
        ]
        summary.update(
            {
                "node_delta": candidate_nodes - baseline_nodes,
                "node_delta_percent": ratio_delta(candidate_nodes, baseline_nodes),
                "candidate_more_nodes": sum(delta > 0 for delta in node_deltas),
                "candidate_fewer_nodes": sum(delta < 0 for delta in node_deltas),
                "same_nodes": sum(delta == 0 for delta in node_deltas),
                "maximum_absolute_node_delta": max(map(abs, node_deltas)),
            }
        )
    else:
        summary["info_nodes_note"] = (
            "With a fixed UCI node cap, Proton reports nodes only for the last "
            "completed iteration. These are not total consumed nodes, so no node "
            "efficiency delta is calculated."
        )
    return summary


def run_pairs(
    candidate: EngineProcess,
    baseline: EngineProcess,
    positions: list[dict[str, Any]],
    go_command: str,
    limit: dict[str, Any],
    trial: int,
    checkpoint: Callable[[list[dict[str, Any]]], None] | None = None,
    starting: Callable[[dict[str, Any]], None] | None = None,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for zero_based, position in enumerate(positions):
        candidate_first = (zero_based + trial - 1) % 2 == 0
        order = (
            (("candidate", candidate), ("baseline", baseline))
            if candidate_first
            else (("baseline", baseline), ("candidate", candidate))
        )
        results: dict[str, dict[str, Any]] = {}
        for name, engine in order:
            if starting is not None:
                starting(
                    {
                        "phase": "search",
                        "trial": trial,
                        "position": position["index"],
                        "engine": name,
                    }
                )
            results[name] = engine.search(position["fen"], go_command, limit)
        rows.append(
            {
                "trial": trial,
                "position": position["index"],
                "source_line": position["line_number"],
                "fen": position["fen"],
                "search_order": [name for name, _ in order],
                "candidate": results["candidate"],
                "baseline": results["baseline"],
            }
        )
        if checkpoint is not None:
            checkpoint(rows)
    return rows


def artifact_payload(label: str, source: Path, digest: str) -> dict[str, Any]:
    return {
        "label": label,
        "path": str(source),
        "sha256": digest,
        "identity": None,
    }


def error_payload(
    error: BaseException, context: dict[str, Any] | None = None
) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "type": type(error).__name__,
        "message": str(error),
    }
    if context:
        payload["context"] = dict(context)
    notes = getattr(error, "__notes__", None)
    if notes:
        payload["notes"] = list(notes)
    return payload


def print_json_report(report: dict[str, Any]) -> None:
    try:
        print(json.dumps(report, indent=2))
    except BrokenPipeError:
        pass


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    candidate_path = args.candidate.resolve()
    baseline_path = args.baseline.resolve()
    openings_path = args.openings.resolve()
    json_path = args.json_path.resolve()
    tool_path = Path(__file__).resolve()
    for path in (candidate_path, baseline_path, openings_path):
        if not path.is_file():
            raise FileNotFoundError(path)
    if candidate_path == baseline_path:
        raise ValueError("candidate and baseline paths must be different")
    if json_path in {candidate_path, baseline_path, openings_path, tool_path}:
        raise ValueError("JSON output must not overwrite an input")

    configuration = engine_configuration(args.hash_mb)
    limit = (
        {"kind": "depth", "value": args.depth}
        if args.depth is not None
        else {"kind": "nodes", "value": args.nodes}
    )
    go_command = f"go {limit['kind']} {limit['value']}"
    report: dict[str, Any] | None = None
    active_context: dict[str, Any] = {"phase": "staging"}

    with tempfile.TemporaryDirectory(prefix="proton-search-compare-") as temporary:
        staging_root = Path(temporary)
        staged_candidate = staging_root / f"candidate{candidate_path.suffix}"
        staged_baseline = staging_root / f"baseline{baseline_path.suffix}"
        staged_openings = staging_root / f"openings{openings_path.suffix}"
        candidate_hash = stage_file(candidate_path, staged_candidate)
        baseline_hash = stage_file(baseline_path, staged_baseline)
        openings_hash = stage_file(openings_path, staged_openings)
        if candidate_hash == baseline_hash:
            raise ValueError("candidate and baseline binaries must have different SHA-256 hashes")
        positions = load_positions(staged_openings, args.positions)
        if sha256_file(staged_openings) != openings_hash:
            raise ValueError("staged openings changed while they were loaded")

        report = {
            "schema_version": 1,
            "comparison_type": "paired_deterministic_search",
            "status": "running",
            "created_utc": utc_now(),
            "completed_utc": None,
            "error": None,
            "limit": limit,
            "hash_mb": args.hash_mb,
            "trials": args.trials,
            "timeout_seconds": args.timeout,
            "requested_positions": len(positions),
            "requested_search_pairs": len(positions) * args.trials,
            "completed_search_pairs": 0,
            "completed_trials": 0,
            "candidate": artifact_payload(
                args.candidate_label, candidate_path, candidate_hash
            ),
            "baseline": artifact_payload(
                args.baseline_label, baseline_path, baseline_hash
            ),
            "engine_options": configuration,
            "openings": {
                "path": str(openings_path),
                "sha256": openings_hash,
                "positions": len(positions),
            },
            "tool": {"path": str(tool_path), "sha256": sha256_file(tool_path)},
            "git_revision": git_revision(ROOT),
            "git_dirty": git_dirty(ROOT),
            "python_version": platform.python_version(),
            "python_chess_version": getattr(chess, "__version__", "unknown"),
            "host": {
                "platform": platform.platform(),
                "system": platform.system(),
                "release": platform.release(),
                "machine": platform.machine(),
                "processor": platform.processor(),
                "logical_cpu_count": os.cpu_count(),
            },
            "order_policy": (
                "candidate first when (zero-based position + zero-based trial) is "
                "even; baseline first otherwise; every position flips order on the "
                "next trial"
            ),
            "lifecycle": (
                "verified private copies; fresh candidate and baseline processes per "
                "trial; identical full-strength options; ucinewgame then isready before "
                "every FEN; hard queue-backed read deadline for every UCI response"
            ),
            "info_nodes_semantics": (
                "depth mode: nodes on the requested completed iteration and suitable "
                "for deterministic tree-size comparison"
                if limit["kind"] == "depth"
                else "nodes mode: nodes on the last completed iteration, not total "
                "nodes consumed against the configured cap"
            ),
            "node_counting_caveat": (
                "Tree-size comparisons assume both revisions count nodes at the same "
                "search boundaries; accounting changes require separate interpretation."
            ),
            "timing_caveat": (
                "Engine and wall times are host-specific supporting measurements; "
                "timing and NPS are excluded from semantic equality."
            ),
            "rows": [],
            "summary": summarize([], limit["kind"]),
        }
        write_json_atomic(json_path, report)

        try:
            rows: list[dict[str, Any]] = []
            expected_profiles: dict[str, tuple[dict[str, str], set[str]]] = {}

            def starting(context: dict[str, Any]) -> None:
                active_context.clear()
                active_context.update(context)

            for trial in range(1, args.trials + 1):
                candidate_engine: EngineProcess | None = None
                baseline_engine: EngineProcess | None = None
                starting({"phase": "launch", "trial": trial})
                try:
                    candidate_engine = EngineProcess(
                        staged_candidate, args.candidate_label, args.timeout
                    )
                    baseline_engine = EngineProcess(
                        staged_baseline, args.baseline_label, args.timeout
                    )
                    starting({"phase": "initialize", "trial": trial,
                              "engine": "candidate"})
                    candidate_engine.initialize(configuration)
                    starting({"phase": "initialize", "trial": trial,
                              "engine": "baseline"})
                    baseline_engine.initialize(configuration)

                    for name, engine in (
                        ("candidate", candidate_engine),
                        ("baseline", baseline_engine),
                    ):
                        profile = (dict(engine.identity), set(engine.options))
                        if name not in expected_profiles:
                            expected_profiles[name] = profile
                            report[name]["identity"] = profile[0]
                            report[name]["advertised_options"] = sorted(profile[1])
                        elif profile != expected_profiles[name]:
                            raise EngineFailure(
                                f"{name} UCI identity or options changed between trials"
                            )
                    write_json_atomic(json_path, report)

                    def checkpoint(trial_rows: list[dict[str, Any]]) -> None:
                        combined = [*rows, *trial_rows]
                        report["rows"] = combined
                        report["completed_search_pairs"] = len(combined)
                        report["completed_trials"] = len(combined) // len(positions)
                        report["summary"] = summarize(combined, limit["kind"])
                        write_json_atomic(json_path, report)

                    trial_rows = run_pairs(
                        candidate_engine,
                        baseline_engine,
                        positions,
                        go_command,
                        limit,
                        trial,
                        checkpoint=checkpoint,
                        starting=starting,
                    )
                    rows.extend(trial_rows)
                    report["completed_trials"] = trial
                finally:
                    pending_error = sys.exc_info()[1]
                    cleanup_errors = close_engines(
                        (candidate_engine, baseline_engine)
                    )
                    if cleanup_errors:
                        details = "; ".join(str(error) for error in cleanup_errors)
                        if pending_error is not None:
                            pending_error.add_note(f"engine cleanup also failed: {details}")
                        else:
                            interrupt = next(
                                (
                                    error
                                    for error in cleanup_errors
                                    if isinstance(error, KeyboardInterrupt)
                                ),
                                None,
                            )
                            if interrupt is not None:
                                raise interrupt
                            raise EngineFailure(
                                f"engine cleanup failed: {details}"
                            ) from cleanup_errors[0]

            active_context.clear()
            active_context["phase"] = "input_reverification"
            for label, source, staged, expected_hash in (
                ("candidate", candidate_path, staged_candidate, candidate_hash),
                ("baseline", baseline_path, staged_baseline, baseline_hash),
                ("openings", openings_path, staged_openings, openings_hash),
                ("tool", tool_path, tool_path, report["tool"]["sha256"]),
            ):
                if sha256_file(staged) != expected_hash or sha256_file(source) != expected_hash:
                    raise ValueError(f"{label} input changed during the comparison")
            report["rows"] = rows
            report["completed_search_pairs"] = len(rows)
            report["completed_trials"] = args.trials
            report["summary"] = summarize(rows, limit["kind"])
            if not report["summary"]["deterministic_across_trials"]:
                active_context.clear()
                active_context["phase"] = "repeatability_check"
                raise EngineFailure(
                    "candidate or baseline produced different semantic results across trials"
                )
            report["status"] = "complete"
            report["completed_utc"] = utc_now()
            report["error"] = None
            write_json_atomic(json_path, report)
            print_json_report(report)
            return 0
        except KeyboardInterrupt as error:
            report["status"] = "interrupted"
            report["completed_utc"] = utc_now()
            report["error"] = error_payload(error, active_context)
            write_json_atomic(json_path, report)
            print(f"comparison interrupted; checkpoint: {json_path}", file=sys.stderr)
            return 130
        except Exception as error:
            report["status"] = "failed"
            report["completed_utc"] = utc_now()
            report["error"] = error_payload(error, active_context)
            write_json_atomic(json_path, report)
            print(f"comparison failed: {error}; checkpoint: {json_path}", file=sys.stderr)
            return 1


if __name__ == "__main__":
    raise SystemExit(main())
