from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess


@dataclass(slots=True)
class PerftCase:
    fen: str
    depth: int
    nodes: int


PERFT_INFO = re.compile(r"^info string perft depth (\d+) nodes (\d+)(?:\s|$)")


def load_cases(path: Path) -> list[PerftCase]:
    cases: list[PerftCase] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fen_text, depth_text, nodes_text = [part.strip() for part in line.split(";")]
            cases.append(
                PerftCase(
                    fen="startpos" if fen_text == "startpos" else fen_text,
                    depth=int(depth_text.split("=", 1)[1]),
                    nodes=int(nodes_text.split("=", 1)[1]),
                )
            )
    return cases


def run_case(engine_path: Path, case: PerftCase) -> int:
    position_cmd = "position startpos" if case.fen == "startpos" else f"position fen {case.fen}"
    payload = f"uci\nisready\n{position_cmd}\nperft {case.depth}\nquit\n"
    result = subprocess.run(
        [str(engine_path)],
        input=payload,
        text=True,
        capture_output=True,
        check=True,
        timeout=60,
    )
    for line in result.stdout.splitlines():
        match = PERFT_INFO.match(line)
        if match and int(match.group(1)) == case.depth:
            return int(match.group(2))
        if line.startswith("perft "):
            return int(line.split()[1])
    raise RuntimeError(f"engine output did not contain perft result:\n{result.stdout}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Run perft regression cases against Proton Chess.")
    parser.add_argument("engine", type=Path)
    parser.add_argument("--cases", type=Path, default=Path("tests/perft_positions.txt"))
    args = parser.parse_args()

    if not args.engine.is_file():
        raise FileNotFoundError(args.engine)

    failures = 0
    for case in load_cases(args.cases):
        actual = run_case(args.engine, case)
        if actual != case.nodes:
            failures += 1
            print(f"FAIL depth={case.depth} expected={case.nodes} actual={actual} fen={case.fen}")
        else:
            print(f"PASS depth={case.depth} nodes={actual} fen={case.fen}")

    if failures:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
