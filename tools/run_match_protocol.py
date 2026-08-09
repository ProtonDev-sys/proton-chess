#!/usr/bin/env python3
"""Verify and run a pinned Proton Chess match protocol."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import shlex
import subprocess
import sys
from pathlib import Path
from typing import Any

import chess

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import estimate_elo  # noqa: E402


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_sha256(value: Any) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(character in "0123456789abcdefABCDEF" for character in value)
    )


def load_protocol(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1:
        raise ValueError("unsupported match protocol schema")
    if payload.get("games_per_level", 0) < 2 or payload["games_per_level"] % 2:
        raise ValueError("games_per_level must be an even number of at least two")
    if not payload.get("opponent_elo"):
        raise ValueError("protocol must contain at least one opponent Elo")
    if payload.get("openings", {}).get("positions") != payload["games_per_level"] // 2:
        raise ValueError("opening position count must equal the number of game pairs")
    time_control = payload.get("time_control", {})
    if time_control.get("base_seconds", 0) <= 0:
        raise ValueError("protocol base_seconds must be positive")
    if time_control.get("increment_seconds", -1) < 0:
        raise ValueError("protocol increment_seconds must be non-negative")
    stockfish_options = payload.get("stockfish", {}).get("options", {})
    if payload["opponent_elo"] != [stockfish_options.get("UCI_Elo")]:
        raise ValueError("Stockfish UCI_Elo must match the sole protocol opponent Elo")
    if stockfish_options.get("Hash") != payload.get("hash_mb"):
        raise ValueError("Stockfish Hash must match protocol hash_mb")
    if stockfish_options.get("Threads") != 1:
        raise ValueError("Stockfish Threads must be one")
    if stockfish_options.get("UCI_LimitStrength") is not True:
        raise ValueError("Stockfish UCI_LimitStrength must be true")
    if not is_sha256(payload.get("proton", {}).get("binary_sha256")):
        raise ValueError("protocol Proton binary_sha256 must be a SHA-256 digest")
    proton_options = payload.get("proton_options", {})
    if proton_options != {
        "Hash": payload.get("hash_mb"),
        "UseBook": False,
        "HumanStyle": False,
    }:
        raise ValueError("Proton options do not match the certification protocol")
    gate = payload.get("pass_condition", {})
    if gate.get("minimum_games") != payload["games_per_level"]:
        raise ValueError("pass-condition game count must match games_per_level")
    if gate.get("score_ci95_low_strictly_greater_than") != 0.5:
        raise ValueError("certification score threshold must be 0.5")
    return payload


def verified_inputs(
    protocol: dict[str, Any],
    repository_root: Path,
    proton: Path,
    stockfish: Path,
) -> tuple[Path, Path, Path]:
    proton = proton.resolve()
    stockfish = stockfish.resolve()
    runner = (repository_root / protocol["runner"]["path"]).resolve()
    openings = (repository_root / protocol["openings"]["path"]).resolve()
    for path in (proton, stockfish, runner, openings):
        if not path.is_file():
            raise FileNotFoundError(path)

    expected_platform = protocol["platform"]
    if platform.system() != expected_platform["system"]:
        raise ValueError(
            f"platform mismatch: expected {expected_platform['system']}, "
            f"got {platform.system()}"
        )
    if platform.machine().lower() != expected_platform["machine"].lower():
        raise ValueError(
            f"machine mismatch: expected {expected_platform['machine']}, "
            f"got {platform.machine()}"
        )
    actual_chess_version = getattr(chess, "__version__", "unknown")
    if actual_chess_version != protocol["python_chess_version"]:
        raise ValueError(
            f"python-chess version mismatch: expected {protocol['python_chess_version']}, "
            f"got {actual_chess_version}"
        )
    expected_runner = protocol["runner"]["sha256"].lower()
    actual_runner = sha256_file(runner)
    if actual_runner != expected_runner:
        raise ValueError(
            f"runner SHA-256 mismatch: expected {expected_runner}, got {actual_runner}"
        )

    expected_proton = protocol["proton"]["binary_sha256"].lower()
    actual_proton = sha256_file(proton)
    if actual_proton != expected_proton:
        raise ValueError(
            f"Proton SHA-256 mismatch: expected {expected_proton}, got {actual_proton}"
        )
    expected_stockfish = protocol["stockfish"]["binary_sha256"].lower()
    actual_stockfish = sha256_file(stockfish)
    if actual_stockfish != expected_stockfish:
        raise ValueError(
            f"Stockfish SHA-256 mismatch: expected {expected_stockfish}, "
            f"got {actual_stockfish}"
        )
    expected_openings = protocol["openings"]["sha256"].lower()
    actual_openings = sha256_file(openings)
    if actual_openings != expected_openings:
        raise ValueError(
            f"opening suite SHA-256 mismatch: expected {expected_openings}, "
            f"got {actual_openings}"
        )
    raw_positions = sum(
        1
        for line in openings.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    )
    loaded_openings = estimate_elo.load_openings(openings)
    actual_positions = len(loaded_openings)
    expected_positions = protocol["openings"]["positions"]
    if raw_positions != expected_positions:
        raise ValueError(
            f"opening line count mismatch: expected {expected_positions}, "
            f"got {raw_positions}"
        )
    if actual_positions != expected_positions:
        raise ValueError(
            f"legal opening count mismatch: expected {expected_positions}, "
            f"got {actual_positions}"
        )
    unique_positions = {
        (opening.fen, tuple(opening.moves)) for opening in loaded_openings
    }
    if len(unique_positions) != actual_positions:
        raise ValueError("opening suite contains duplicate normalized positions")
    if protocol.get("require_clean_git"):
        status = subprocess.run(
            ["git", "status", "--porcelain", "--untracked-files=no"],
            cwd=repository_root,
            text=True,
            capture_output=True,
            check=False,
            timeout=10,
        )
        if status.returncode != 0:
            raise ValueError("Git status preflight failed")
        if status.stdout.strip():
            raise ValueError("certification protocol requires a clean Git worktree")
    return runner, openings, proton


def build_command(
    protocol: dict[str, Any],
    repository_root: Path,
    proton: Path,
    stockfish: Path,
    json_path: Path,
) -> list[str]:
    runner, openings, proton = verified_inputs(
        protocol, repository_root, proton, stockfish
    )
    time_control = protocol["time_control"]
    command = [sys.executable, str(runner), str(proton), str(stockfish.resolve())]
    for rating in protocol["opponent_elo"]:
        command.extend(("--opponent-elo", str(rating)))
    command.extend((
        "--games", str(protocol["games_per_level"]),
        "--base-seconds", str(time_control["base_seconds"]),
        "--increment", str(time_control["increment_seconds"]),
        "--watchdog-grace", str(time_control["watchdog_grace_seconds"]),
        "--max-plies", str(protocol["max_plies"]),
        "--hash", str(protocol["hash_mb"]),
        "--seed", str(protocol["seed"]),
        "--openings", str(openings),
        "--json", str(json_path.resolve()),
    ))
    return command


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run estimate_elo.py only after pinned match inputs are verified."
    )
    parser.add_argument("protocol", type=Path)
    parser.add_argument("proton", type=Path)
    parser.add_argument("stockfish", type=Path)
    parser.add_argument("--json", type=Path, required=True, dest="json_path")
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        protocol_path = args.protocol.resolve()
        protocol = load_protocol(protocol_path)
        repository_root = protocol_path.parents[1]
        command = build_command(
            protocol,
            repository_root,
            args.proton,
            args.stockfish,
            args.json_path,
        )
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    print(shlex.join(command))
    if args.dry_run:
        return 0
    return subprocess.run(command, cwd=repository_root, check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
