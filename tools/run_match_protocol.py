#!/usr/bin/env python3
"""Verify and run a pinned Proton Chess match protocol."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
from collections.abc import Iterator
from contextlib import contextmanager
from pathlib import Path, PurePosixPath, PureWindowsPath
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


def is_hex_digest(value: Any, length: int) -> bool:
    return (
        isinstance(value, str)
        and len(value) == length
        and all(character in "0123456789abcdefABCDEF" for character in value)
    )


def require_object(payload: dict[str, Any], key: str) -> dict[str, Any]:
    value = payload.get(key)
    if not isinstance(value, dict):
        raise ValueError(f"protocol {key} must be an object")
    return value


def require_string(payload: dict[str, Any], key: str, label: str) -> str:
    value = payload.get(key)
    if not isinstance(value, str) or not value.strip():
        raise ValueError(f"protocol {label} must be a non-empty string")
    return value


def require_int(payload: dict[str, Any], key: str, label: str) -> int:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"protocol {label} must be an integer")
    return value


def require_number(payload: dict[str, Any], key: str, label: str) -> float:
    value = payload.get(key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"protocol {label} must be a number")
    try:
        number = float(value)
    except OverflowError as error:
        raise ValueError(f"protocol {label} must be a finite number") from error
    if not math.isfinite(number):
        raise ValueError(f"protocol {label} must be a finite number")
    return number


def require_digest(
    payload: dict[str, Any], key: str, label: str, length: int = 64
) -> str:
    value = payload.get(key)
    if not is_hex_digest(value, length):
        raise ValueError(f"protocol {label} must be a {length * 4}-bit hex digest")
    return value.lower()


def require_repository_path(payload: dict[str, Any], key: str, label: str) -> str:
    value = require_string(payload, key, label)
    posix_path = PurePosixPath(value)
    windows_path = PureWindowsPath(value)
    if (
        posix_path.is_absolute()
        or windows_path.is_absolute()
        or bool(windows_path.anchor)
        or ".." in posix_path.parts
        or ".." in windows_path.parts
    ):
        raise ValueError(f"protocol {label} must be a repository-relative path")
    return value


def resolve_repository_input(
    repository_root: Path, relative_path: str, label: str
) -> Path:
    root = repository_root.resolve()
    resolved = (root / relative_path).resolve()
    if not resolved.is_relative_to(root):
        raise ValueError(f"protocol {label} resolves outside the repository")
    return resolved


def load_protocol(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(payload, dict):
        raise ValueError("match protocol must be a JSON object")
    schema_version = payload.get("schema_version")
    if (
        isinstance(schema_version, bool)
        or not isinstance(schema_version, int)
        or schema_version != 1
    ):
        raise ValueError("unsupported match protocol schema")
    require_string(payload, "name", "name")
    runner = require_object(payload, "runner")
    require_repository_path(runner, "path", "runner.path")
    require_digest(runner, "sha256", "runner.sha256")
    require_string(payload, "python_chess_version", "python_chess_version")
    expected_platform = require_object(payload, "platform")
    require_string(expected_platform, "system", "platform.system")
    require_string(expected_platform, "machine", "platform.machine")
    if not isinstance(payload.get("require_clean_git"), bool):
        raise ValueError("protocol require_clean_git must be a boolean")

    games_per_level = require_int(payload, "games_per_level", "games_per_level")
    if games_per_level < 2 or games_per_level % 2:
        raise ValueError("games_per_level must be an even number of at least two")
    opponent_elo = payload.get("opponent_elo")
    if (
        not isinstance(opponent_elo, list)
        or not opponent_elo
        or any(
            isinstance(value, bool) or not isinstance(value, int) or value <= 0
            for value in opponent_elo
        )
    ):
        raise ValueError("protocol must contain at least one opponent Elo")

    time_control = require_object(payload, "time_control")
    base_seconds = require_number(time_control, "base_seconds", "time_control.base_seconds")
    increment_seconds = require_number(
        time_control, "increment_seconds", "time_control.increment_seconds"
    )
    watchdog_grace = require_number(
        time_control,
        "watchdog_grace_seconds",
        "time_control.watchdog_grace_seconds",
    )
    if base_seconds <= 0:
        raise ValueError("protocol base_seconds must be positive")
    if increment_seconds < 0:
        raise ValueError("protocol increment_seconds must be non-negative")
    if watchdog_grace < 0:
        raise ValueError("protocol watchdog_grace_seconds must be non-negative")
    if require_int(payload, "max_plies", "max_plies") <= 0:
        raise ValueError("protocol max_plies must be positive")
    hash_mb = require_int(payload, "hash_mb", "hash_mb")
    if hash_mb <= 0:
        raise ValueError("protocol hash_mb must be positive")
    require_int(payload, "seed", "seed")
    require_string(payload, "pairing", "pairing")
    require_string(payload, "adjudication", "adjudication")

    openings = require_object(payload, "openings")
    require_repository_path(openings, "path", "openings.path")
    require_digest(openings, "sha256", "openings.sha256")
    positions = require_int(openings, "positions", "openings.positions")
    if positions != games_per_level // 2:
        raise ValueError("opening position count must equal the number of game pairs")
    require_string(openings, "source_repository", "openings.source_repository")
    require_digest(openings, "source_commit", "openings.source_commit", 40)
    require_string(openings, "source_archive", "openings.source_archive")
    require_digest(
        openings, "source_archive_sha256", "openings.source_archive_sha256"
    )
    require_digest(openings, "source_epd_sha256", "openings.source_epd_sha256")
    require_string(openings, "license", "openings.license")

    stockfish = require_object(payload, "stockfish")
    for key in ("name", "release_tag", "release_url", "asset_name", "binary_name"):
        require_string(stockfish, key, f"stockfish.{key}")
    require_digest(stockfish, "asset_sha256", "stockfish.asset_sha256")
    require_digest(stockfish, "binary_sha256", "stockfish.binary_sha256")
    stockfish_options = require_object(stockfish, "options")
    if opponent_elo != [require_int(stockfish_options, "UCI_Elo", "stockfish.options.UCI_Elo")]:
        raise ValueError("Stockfish UCI_Elo must match the sole protocol opponent Elo")
    if require_int(stockfish_options, "Hash", "stockfish.options.Hash") != hash_mb:
        raise ValueError("Stockfish Hash must match protocol hash_mb")
    if require_int(stockfish_options, "Threads", "stockfish.options.Threads") != 1:
        raise ValueError("Stockfish Threads must be one")
    if stockfish_options.get("UCI_LimitStrength") is not True:
        raise ValueError("Stockfish UCI_LimitStrength must be true")
    proton = require_object(payload, "proton")
    require_string(proton, "name", "proton.name")
    require_digest(proton, "binary_sha256", "Proton binary_sha256")
    proton_options = require_object(payload, "proton_options")
    if proton_options != {
        "Hash": hash_mb,
        "UseBook": False,
        "HumanStyle": False,
    }:
        raise ValueError("Proton options do not match the certification protocol")
    gate = require_object(payload, "pass_condition")
    if require_int(gate, "minimum_games", "pass_condition.minimum_games") != games_per_level:
        raise ValueError("pass-condition game count must match games_per_level")
    if require_number(
        gate,
        "score_ci95_low_strictly_greater_than",
        "pass_condition.score_ci95_low_strictly_greater_than",
    ) != 0.5:
        raise ValueError("certification score threshold must be 0.5")
    return payload


def verified_inputs(
    protocol: dict[str, Any],
    repository_root: Path,
    proton: Path,
    stockfish: Path,
) -> tuple[Path, Path, Path, Path]:
    proton = proton.resolve()
    stockfish = stockfish.resolve()
    runner = resolve_repository_input(
        repository_root, protocol["runner"]["path"], "runner.path"
    )
    openings = resolve_repository_input(
        repository_root, protocol["openings"]["path"], "openings.path"
    )
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
    return runner, openings, proton, stockfish


@contextmanager
def staged_inputs(
    protocol: dict[str, Any],
    repository_root: Path,
    proton: Path,
    stockfish: Path,
) -> Iterator[tuple[Path, Path, Path, Path]]:
    runner, openings, proton, stockfish = verified_inputs(
        protocol, repository_root, proton, stockfish
    )
    with tempfile.TemporaryDirectory(
        prefix=".proton-match-", dir=repository_root
    ) as temporary:
        stage_root = Path(temporary)
        staged_runner = stage_root / "tools" / runner.name
        staged_openings = stage_root / "openings" / openings.name
        staged_proton = stage_root / "engines" / f"proton{proton.suffix}"
        staged_stockfish = stage_root / "engines" / f"stockfish{stockfish.suffix}"
        staged = (
            (runner, staged_runner, protocol["runner"]["sha256"], "runner"),
            (
                openings,
                staged_openings,
                protocol["openings"]["sha256"],
                "opening suite",
            ),
            (proton, staged_proton, protocol["proton"]["binary_sha256"], "Proton"),
            (
                stockfish,
                staged_stockfish,
                protocol["stockfish"]["binary_sha256"],
                "Stockfish",
            ),
        )
        for source, destination, expected_hash, label in staged:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
            actual_hash = sha256_file(destination)
            if actual_hash != expected_hash.lower():
                raise ValueError(
                    f"staged {label} SHA-256 mismatch: "
                    f"expected {expected_hash.lower()}, got {actual_hash}"
                )
        yield staged_runner, staged_openings, staged_proton, staged_stockfish


def build_command(
    protocol: dict[str, Any],
    runner: Path,
    openings: Path,
    proton: Path,
    stockfish: Path,
    json_path: Path,
) -> list[str]:
    time_control = protocol["time_control"]
    command = [sys.executable, str(runner), str(proton), str(stockfish)]
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
        with staged_inputs(
            protocol, repository_root, args.proton, args.stockfish
        ) as (runner, openings, proton, stockfish):
            command = build_command(
                protocol,
                runner,
                openings,
                proton,
                stockfish,
                args.json_path,
            )
            print(shlex.join(command))
            if args.dry_run:
                return 0
            return subprocess.run(command, cwd=repository_root, check=False).returncode
    except (FileNotFoundError, json.JSONDecodeError, OSError, ValueError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
