#!/usr/bin/env python3
"""Fetch a digest-verified official Stockfish release without vendoring it."""

import argparse
import hashlib
import io
import json
from pathlib import Path
import platform
import tarfile
import urllib.request
import zipfile


def fetch(url: str) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "proton-chess-strength-tests"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tag", default="latest", help="official release tag, e.g. sf_19")
    parser.add_argument("--directory", type=Path, default=Path("external"))
    args = parser.parse_args()
    endpoint = "latest" if args.tag == "latest" else f"tags/{args.tag}"
    release = json.loads(fetch(f"https://api.github.com/repos/official-stockfish/Stockfish/releases/{endpoint}"))
    if release["draft"] or release["prerelease"]:
        raise ValueError("expected a stable, published release")
    operating_system = {"Windows": "windows", "Linux": "linux", "Darwin": "macos"}[platform.system()]
    machine = platform.machine().lower()
    architecture = {"amd64": "x86-64", "x86_64": "x86-64", "aarch64": "arm64", "arm64": "arm64"}[machine]
    asset_name = ("stockfish-macos-universal.tar.gz" if operating_system == "macos" else
                  f"stockfish-{operating_system}-{architecture}-universal" +
                  (".zip" if operating_system == "windows" else ".tar.gz"))
    asset = next(asset for asset in release["assets"] if asset["name"] == asset_name)
    archive = fetch(asset["browser_download_url"])
    digest = "sha256:" + hashlib.sha256(archive).hexdigest()
    if asset.get("digest") != digest or len(archive) != asset["size"]:
        raise ValueError("release archive does not match the official size and SHA-256 digest")
    destination = (args.directory / release["tag_name"]).resolve()
    wanted = {"Copying.txt", "AUTHORS", "README.md"}
    files = {}
    if asset_name.endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(archive)) as bundle:
            for member in bundle.infolist():
                name = Path(member.filename).name
                if not member.is_dir() and (name in wanted or name.endswith(".exe")):
                    files[name] = bundle.read(member)
    else:
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:gz") as bundle:
            for member in bundle.getmembers():
                name = Path(member.name).name
                if member.isfile() and (name in wanted or name.startswith("stockfish-")):
                    source = bundle.extractfile(member)
                    if source is not None:
                        files[name] = source.read()
    binaries = [name for name in files if name not in wanted]
    if len(binaries) != 1 or "Copying.txt" not in files:
        raise ValueError("release must contain one engine and its license")
    for name, contents in files.items():
        path = destination / name
        if path.exists() and path.read_bytes() != contents:
            raise FileExistsError(f"refusing to overwrite different file: {path}")
    destination.mkdir(parents=True, exist_ok=True)
    for name, contents in files.items():
        path = destination / name
        path.write_bytes(contents)
        if name in binaries and operating_system != "windows":
            path.chmod(path.stat().st_mode | 0o111)
    metadata = {"tag": release["tag_name"], "published_at": release["published_at"],
                "release_url": release["html_url"], "asset": asset_name,
                "archive_digest": digest, "binary": str(destination / binaries[0]),
                "binary_sha256": hashlib.sha256(files[binaries[0]]).hexdigest()}
    (destination / "release.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
