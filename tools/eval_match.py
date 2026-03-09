from __future__ import annotations

import argparse
import json
from pathlib import Path
import shlex


def build_fastchess_command(config: dict) -> str:
    engine = config["engine"]
    stockfish = config["stockfish"]
    openings = config["openings"]
    tc = config["time_control"]
    hardware = config["hardware"]

    proto = [
        "fastchess",
        "-engine",
        f"name={engine['name']}",
        f"cmd={engine['cmd']}",
        f"option.Threads={hardware['engine_threads']}",
        f"option.Hash={hardware['hash_mb']}",
        f"option.Backend={engine['backend']}",
        "-engine",
        f"name={stockfish['name']}",
        f"cmd={stockfish['cmd']}",
        f"option.Threads={hardware['stockfish_threads']}",
        f"option.Hash={hardware['hash_mb']}",
        "-openings",
        f"file={openings['file']}",
        f"format={openings['format']}",
        f"order={openings['order']}",
        f"plies={openings['plies']}",
        "-each",
        f"tc={tc}",
        "proto=uci",
        "timemargin=25",
        "-rounds",
        str(config["rounds"]),
        "-repeat",
        "-sprt",
        f"elo0={config['sprt']['elo0']}",
        f"elo1={config['sprt']['elo1']}",
        f"alpha={config['sprt']['alpha']}",
        f"beta={config['sprt']['beta']}",
    ]
    return " ".join(shlex.quote(part) for part in proto)


def main() -> None:
    parser = argparse.ArgumentParser(description="Render a fair fastchess command from JSON config.")
    parser.add_argument("config", type=Path)
    args = parser.parse_args()

    with args.config.open("r", encoding="utf-8") as handle:
        config = json.load(handle)
    print(build_fastchess_command(config))


if __name__ == "__main__":
    main()
