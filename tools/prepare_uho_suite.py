#!/usr/bin/env python3
"""Derive the pinned Proton match suite from the official Stockfish UHO book."""

from __future__ import annotations

import argparse
import hashlib
import heapq
from dataclasses import dataclass
from pathlib import Path

import chess


SOURCE_NAME = "UHO_Lichess_4852_v1.epd"
SOURCE_REPOSITORY = "https://github.com/official-stockfish/books"
SOURCE_COMMIT = "65815ccdbc7727cd4f6aee252ba8f67fb740e92f"
SOURCE_ARCHIVE_URL = (
    "https://raw.githubusercontent.com/official-stockfish/books/"
    f"{SOURCE_COMMIT}/{SOURCE_NAME}.zip"
)
SOURCE_ARCHIVE_SHA256 = "4e298f11e8acfa106babe02968f2e61582145e7874c59284690b20b9650e0e07"
SOURCE_EPD_SHA256 = "7a7f6470615a69c6cf23d565417701d38732876f480af90d67b42abade35644a"
SOURCE_EPD_SHA384_SRI = "QHAU1P3LurcJr7UTRI7HZCVFsoYBWC3OTsBqZY/FfQA6VQo3MmECWtByB4gVACW5"
SELECTION_SEED = "proton-stockfish3000-20260809-v1"
SELECTION_SIZE = 200


@dataclass(frozen=True)
class GenerationResult:
    source_positions: int
    candidate_pool_size: int
    rejected_candidates_before_selection: int
    selected_positions: int
    output_sha256: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selection_rank(seed: str, fen: str) -> int:
    payload = seed.encode("utf-8") + b"\0" + fen.encode("ascii")
    return int.from_bytes(hashlib.sha256(payload).digest(), "big")


def select_positions(
    source: Path, count: int, seed: str
) -> tuple[list[str], int, int, int]:
    if count < 1:
        raise ValueError("selection count must be positive")

    candidate_limit = max(count * 4, count + 100)
    heap: list[tuple[int, str]] = []
    candidate_lines: set[str] = set()
    source_positions = 0
    with source.open("r", encoding="utf-8") as handle:
        for raw_line in handle:
            text = raw_line.strip()
            if not text or text.startswith("#"):
                continue
            source_positions += 1
            if text in candidate_lines:
                continue
            rank = selection_rank(seed, text)
            entry = (-rank, text)
            if len(heap) < candidate_limit:
                heapq.heappush(heap, entry)
                candidate_lines.add(text)
            elif rank < -heap[0][0]:
                removed = heapq.heapreplace(heap, entry)
                candidate_lines.remove(removed[1])
                candidate_lines.add(text)

    ranked_candidates = sorted((-negative_rank, text) for negative_rank, text in heap)
    positions: list[str] = []
    selected_fens: set[str] = set()
    rejected_candidates = 0
    for _, text in ranked_candidates:
        try:
            board = chess.Board(text)
        except ValueError:
            rejected_candidates += 1
            continue
        if not board.is_valid() or board.is_game_over(claim_draw=True):
            rejected_candidates += 1
            continue
        fen = board.fen(en_passant="fen")
        if fen in selected_fens:
            rejected_candidates += 1
            continue
        positions.append(fen)
        selected_fens.add(fen)
        if len(positions) == count:
            break

    if len(positions) != count:
        raise ValueError(
            f"only {len(positions)} usable unique positions found in "
            f"the top {len(ranked_candidates)} deterministic candidates"
        )
    return positions, source_positions, len(ranked_candidates), rejected_candidates


def generate_suite(
    source: Path,
    output: Path,
    *,
    expected_source_sha256: str = SOURCE_EPD_SHA256,
    count: int = SELECTION_SIZE,
    seed: str = SELECTION_SEED,
) -> GenerationResult:
    actual_source_sha256 = sha256_file(source)
    if actual_source_sha256.lower() != expected_source_sha256.lower():
        raise ValueError(
            f"source SHA-256 mismatch: expected {expected_source_sha256}, "
            f"got {actual_source_sha256}"
        )

    positions, source_positions, candidate_pool_size, rejected_candidates = select_positions(
        source, count, seed
    )
    lines = [
        "# Proton Chess Stockfish-3000 certification suite",
        f"# source: {SOURCE_REPOSITORY}/blob/{SOURCE_COMMIT}/{SOURCE_NAME}.zip",
        f"# source archive SHA-256: {SOURCE_ARCHIVE_SHA256}",
        f"# source EPD SHA-256: {actual_source_sha256}",
        f"# source EPD SHA-384 SRI: {SOURCE_EPD_SHA384_SRI}",
        "# source license: CC0-1.0",
        f"# selection seed: {seed}",
        "# selection: lowest SHA-256(seed NUL exact-source-FEN) ranks",
        f"# source positions: {source_positions}",
        f"# deterministic candidate pool size: {candidate_pool_size}",
        f"# rejected candidates before selection completed: {rejected_candidates}",
        f"# selected positions: {len(positions)}",
        "",
        *positions,
    ]
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return GenerationResult(
        source_positions=source_positions,
        candidate_pool_size=candidate_pool_size,
        rejected_candidates_before_selection=rejected_candidates,
        selected_positions=len(positions),
        output_sha256=sha256_file(output),
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Verify the official UHO EPD and derive Proton's pinned 200-position suite."
    )
    parser.add_argument("source", type=Path, help="uncompressed UHO_Lichess_4852_v1.epd")
    parser.add_argument(
        "--output",
        type=Path,
        default=Path("openings/uho_lichess_4852_v1_200.epd"),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    result = generate_suite(args.source.resolve(), args.output.resolve())
    print(
        f"selected {result.selected_positions} from {result.source_positions} source positions; "
        f"candidate pool {result.candidate_pool_size} "
        f"({result.rejected_candidates_before_selection} rejected before selection); "
        f"output SHA-256 {result.output_sha256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
