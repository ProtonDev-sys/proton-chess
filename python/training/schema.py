from __future__ import annotations

from dataclasses import asdict, dataclass
import json
from pathlib import Path
from typing import Iterable, Iterator


@dataclass(slots=True)
class TrainingRecord:
    fen: str
    side_to_move: str
    game_result: float
    teacher_cp: int
    teacher_wdl: tuple[float, float, float]
    teacher_policy: list[tuple[str, float]]
    source_tag: str

    def to_json(self) -> str:
        return json.dumps(asdict(self), separators=(",", ":"))

    @classmethod
    def from_json(cls, raw: str) -> "TrainingRecord":
        payload = json.loads(raw)
        payload["teacher_wdl"] = tuple(payload["teacher_wdl"])
        payload["teacher_policy"] = [tuple(item) for item in payload["teacher_policy"]]
        return cls(**payload)


def load_jsonl(path: Path) -> Iterator[TrainingRecord]:
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line:
                continue
            yield TrainingRecord.from_json(line)


def write_jsonl(path: Path, records: Iterable[TrainingRecord]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as handle:
        for record in records:
            handle.write(record.to_json())
            handle.write("\n")
