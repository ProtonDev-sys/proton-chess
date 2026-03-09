from __future__ import annotations

import argparse
from pathlib import Path
from statistics import mean

if __package__ in {None, ""}:
    import sys

    sys.path.append(str(Path(__file__).resolve().parents[1]))
    from training.schema import load_jsonl
else:
    from .schema import load_jsonl


def summarize_dataset(path: Path) -> dict[str, float]:
    records = list(load_jsonl(path))
    if not records:
        raise ValueError(f"no records found in {path}")
    return {
        "count": float(len(records)),
        "mean_teacher_cp": mean(record.teacher_cp for record in records),
        "mean_result": mean(record.game_result for record in records),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description="Summarize Proton Chess training data.")
    parser.add_argument("dataset", type=Path)
    args = parser.parse_args()

    summary = summarize_dataset(args.dataset)
    for key, value in summary.items():
        print(f"{key}={value}")


if __name__ == "__main__":
    main()
