#!/usr/bin/env python3
"""Validate and merge fixed-node Stockfish calibration shards."""

from __future__ import annotations

import argparse
import json
import platform
import sys
from dataclasses import asdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import calibrate_stockfish  # noqa: E402
from tools import estimate_elo  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate and merge disjoint Stockfish calibration shards."
    )
    parser.add_argument("reports", type=Path, nargs="+")
    parser.add_argument("--json", type=Path, dest="json_path", required=True)
    return parser.parse_args()


def invariant(payload: dict[str, Any]) -> tuple[Any, ...]:
    return (
        payload["schema_version"],
        payload["match_type"],
        payload["seed"],
        payload["max_plies"],
        payload["hash_mb"],
        json.dumps(payload["time_control"], sort_keys=True),
        payload["proton"]["sha256"],
        payload["stockfish"]["sha256"],
        json.dumps(payload["opponent"], sort_keys=True),
        json.dumps(payload["proton_options"], sort_keys=True),
        json.dumps(payload["stockfish_options"], sort_keys=True),
        payload["openings"]["sha256"],
        payload["tool"]["sha256"],
        payload["match_core"]["sha256"],
        payload["proton_source_commit"],
        payload["stockfish_source_ref"],
    )


def load_shard(path: Path) -> dict[str, Any]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    if payload.get("schema_version") != 1:
        raise ValueError(f"unsupported shard schema: {path}")
    if payload.get("match_type") != "stockfish_fixed_node_calibration_shard":
        raise ValueError(f"not a Stockfish calibration shard: {path}")
    if payload.get("status") != "complete":
        raise ValueError(f"incomplete calibration shard: {path}")
    if payload["result"]["games"] != payload["pair_count"] * 2:
        raise ValueError(f"shard game count is inconsistent: {path}")
    return payload


def record_from_payload(payload: dict[str, Any]) -> estimate_elo.GameRecord:
    return estimate_elo.GameRecord(**payload)


def merge_shards(paths: list[Path]) -> dict[str, Any]:
    if not paths:
        raise ValueError("at least one shard is required")
    loaded = [(path.resolve(), load_shard(path.resolve())) for path in paths]
    reference = invariant(loaded[0][1])
    for path, payload in loaded[1:]:
        if invariant(payload) != reference:
            raise ValueError(f"calibration shard invariants differ: {path}")

    seen_pairs: set[int] = set()
    seen_openings: set[int] = set()
    records: list[estimate_elo.GameRecord] = []
    sources: list[dict[str, Any]] = []
    for path, payload in loaded:
        shard_pairs = {
            int(record["pair"]) for record in payload["result"]["records"]
        }
        overlap = seen_pairs & shard_pairs
        if overlap:
            raise ValueError(f"calibration pair overlap: {sorted(overlap)}")
        seen_pairs |= shard_pairs

        opening_indices = set(int(value) for value in payload["selected_opening_indices"])
        opening_overlap = seen_openings & opening_indices
        if opening_overlap:
            raise ValueError(
                f"calibration opening overlap: {sorted(opening_overlap)}"
            )
        seen_openings |= opening_indices
        records.extend(
            record_from_payload(record)
            for record in payload["result"]["records"]
        )
        sources.append({
            "path": str(path),
            "sha256": estimate_elo.sha256_file(path),
            "pair_offset": payload["pair_offset"],
            "pair_count": payload["pair_count"],
        })

    records.sort(key=lambda record: (
        record.pair, 0 if record.proton_color == "white" else 1
    ))
    first = loaded[0][1]
    opponent = calibrate_stockfish.OpponentLevel(**first["opponent"])
    result = calibrate_stockfish.summarize_records(opponent, records)
    tool_path = Path(__file__).resolve()
    completed = datetime.now(timezone.utc).isoformat(timespec="seconds")
    return {
        "schema_version": 1,
        "match_type": "stockfish_fixed_node_calibration_merged",
        "status": "complete",
        "completed_utc": completed,
        "seed": first["seed"],
        "pair_count": result.pair_count,
        "games": result.games,
        "max_plies": first["max_plies"],
        "hash_mb": first["hash_mb"],
        "time_control": first["time_control"],
        "proton": first["proton"],
        "stockfish": first["stockfish"],
        "stockfish_uci_elo_range": first["stockfish_uci_elo_range"],
        "opponent": first["opponent"],
        "proton_options": first["proton_options"],
        "stockfish_options": first["stockfish_options"],
        "openings": first["openings"],
        "opening_order": first["opening_order"],
        "selected_opening_indices": sorted(seen_openings),
        "proton_source_commit": first["proton_source_commit"],
        "stockfish_source_ref": first["stockfish_source_ref"],
        "shard_tool": first["tool"],
        "match_core": first["match_core"],
        "merge_tool": {
            "path": str(tool_path),
            "sha256": estimate_elo.sha256_file(tool_path),
        },
        "python_version": platform.python_version(),
        "sources": sorted(sources, key=lambda source: source["pair_offset"]),
        "confidence_level": 0.95,
        "confidence_assumptions": [
            "The two games within each color-swapped opening pair are one observation.",
            "All shards use disjoint openings from one seeded permutation.",
            "The result is specific to the pinned binaries, node budget, opening suite, "
            "options, and adjudication; Stockfish UCI_Elo is a limiter setting, not "
            "a universal human rating.",
        ],
        "confidently_beats_level": result.significant_above_50,
        "confidently_dominates_level": (
            result.significant_above_50 and result.observed_score_at_least_70
        ),
        "result": asdict(result),
    }


def main() -> int:
    args = parse_args()
    payload = merge_shards(args.reports)
    estimate_elo.write_json_atomic(args.json_path.resolve(), payload)
    print(json.dumps(payload, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
