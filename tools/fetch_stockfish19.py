#!/usr/bin/env python3
"""Download the hash-pinned official Linux x86-64 Stockfish 19 reference.

Only the executable is copied out; archive paths, links and permissions are not
extracted onto the host. The reference stays external to the MIT-licensed engine.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import platform
import tarfile
import tempfile
import urllib.request
from pathlib import Path

RELEASE = "sf_19"
ASSET = "stockfish-linux-x86-64-universal.tar.gz"
URL = f"https://github.com/official-stockfish/Stockfish/releases/download/{RELEASE}/{ASSET}"
# Official immutable release asset 545574859, published 2026-09-05.
ARCHIVE_SHA256 = "9defc0d4e55d49c65a6d042f3e571a39fcea499ade6dbe741b53b8c65e03611f"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def extract_reference(archive: Path, destination: Path, expected_sha256: str = ARCHIVE_SHA256) -> dict:
    if sha256_file(archive) != expected_sha256:
        raise ValueError("Stockfish release archive SHA-256 mismatch")
    if destination.exists():
        raise FileExistsError(f"refusing to overwrite {destination}")
    with tarfile.open(archive, "r:gz") as source:
        candidates = []
        for member in source.getmembers():
            if not member.isfile() or not Path(member.name).name.startswith("stockfish"):
                continue
            if not 4 <= member.size <= 512 * 1024 * 1024:
                continue
            handle = source.extractfile(member)
            if handle is not None:
                with handle:
                    if handle.read(4) == b"\x7fELF":
                        candidates.append(member)
        if len(candidates) != 1:
            raise ValueError("expected exactly one Stockfish ELF executable in the official archive")
        destination.parent.mkdir(parents=True, exist_ok=True)
        handle = source.extractfile(candidates[0])
        if handle is None:
            raise ValueError("reference executable is unreadable")
        with handle, destination.open("xb") as output:
            for chunk in iter(lambda: handle.read(1024 * 1024), b""):
                output.write(chunk)
    destination.chmod(0o755)
    return {"release_tag": RELEASE, "asset": ASSET, "url": URL,
            "archive_sha256": expected_sha256, "binary_sha256": sha256_file(destination)}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    try:
        if platform.system() != "Linux" or platform.machine().lower() not in ("x86_64", "amd64"):
            raise ValueError("this pinned reference asset is for Linux x86-64 only")
        args.directory.mkdir(parents=True, exist_ok=True)
        binary = args.directory / "stockfish19"
        manifest = args.directory / "stockfish19.json"
        if binary.exists() or manifest.exists():
            raise FileExistsError("use a new output directory; existing references are never silently replaced")
        with tempfile.TemporaryDirectory(prefix="sf19-download-") as temp:
            archive = Path(temp) / ASSET
            request = urllib.request.Request(URL, headers={"User-Agent": "proton-chess-reference/1"})
            with urllib.request.urlopen(request, timeout=120) as response, archive.open("xb") as output:
                total = 0
                for chunk in iter(lambda: response.read(1024 * 1024), b""):
                    total += len(chunk)
                    if total > 256 * 1024 * 1024:
                        raise ValueError("reference download exceeded the size limit")
                    output.write(chunk)
            metadata = extract_reference(archive, binary)
        manifest.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(metadata, indent=2))
    except (ValueError, OSError, tarfile.TarError) as error:
        parser.exit(2, f"reference setup failed: {error}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
