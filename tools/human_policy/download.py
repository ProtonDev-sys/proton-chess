"""Capped, auditable archive-prefix acquisition; never read beyond CAP bytes."""
import argparse
import hashlib
import importlib.metadata
import json
import pathlib
import platform
import time
import urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/human-research'
URL = 'https://database.lichess.org/standard/lichess_db_standard_rated_2026-09.pgn.zst'
CAP = 32 * 1024 * 1024


def main():
    global OUT, URL, CAP
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('results/human-research'))
    parser.add_argument('--url', default=URL)
    parser.add_argument('--max-mib', type=int, choices=range(1, 33), default=32)
    parser.add_argument('--expected-sha256', default='d629023c6ceb487afcd676cc840e744a1f97b8f8782e9f0af3f846998fa205f6', help='Empty string skips hash enforcement for a different sample.')
    args = parser.parse_args()
    OUT, URL, CAP = args.out_dir.resolve(), args.url, args.max_mib * 1024 * 1024
    OUT.mkdir(parents=True, exist_ok=True)
    started = time.time()
    req = urllib.request.Request(URL, headers={
        'Range': f'bytes=0-{CAP - 1}', 'User-Agent': 'ProtonChessResearch/0.1 (bounded public CC0 sample)',
        'Accept-Encoding': 'identity',
    })
    manifest = {'url': URL, 'requested_range': f'bytes=0-{CAP - 1}', 'cap_bytes': CAP,
                'started_utc': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                'python': platform.python_version()}
    digest = hashlib.sha256()
    size = 0
    with urllib.request.urlopen(req, timeout=30) as response, (OUT / 'prefix.pgn.zst').open('wb') as target:
        manifest.update(status=response.status, final_url=response.url, headers=dict(response.headers.items()))
        while size < CAP:
            chunk = response.read(min(65536, CAP - size))
            if not chunk:
                break
            digest.update(chunk)
            target.write(chunk)
            size += len(chunk)
        # Closing immediately is deliberate even for HTTP 200 / ignored Range.
    manifest.update(compressed_bytes=size, compressed_sha256=digest.hexdigest(),
                    capped_at_limit=size == CAP, elapsed_seconds=time.time() - started)
    (OUT / 'download.json').write_text(json.dumps(manifest, indent=2))
    if args.expected_sha256 and manifest['compressed_sha256'] != args.expected_sha256:
        raise ValueError('Compressed prefix SHA-256 does not match expected hash; do not use this sample.')
    print(json.dumps(manifest, indent=2), flush=True)
    # Capture first-party license statement as a small, separate HTML artifact.
    license_req = urllib.request.Request('https://database.lichess.org/', headers={'User-Agent': 'ProtonChessResearch/0.1'})
    with urllib.request.urlopen(license_req, timeout=30) as response:
        html = response.read(512 * 1024)
        (OUT / 'database-license-source.html').write_bytes(html)
        text = html.decode('utf-8', errors='replace')
        lines = [line.strip() for line in text.splitlines() if any(s in line.lower() for s in ('cc0', 'creativecommons', 'license', 'copyright'))]
        license_manifest = {'url': response.url, 'status': response.status, 'sha256': hashlib.sha256(html).hexdigest(),
                            'bytes': len(html), 'matching_lines': lines}
        (OUT / 'license.json').write_text(json.dumps(license_manifest, indent=2))
        print(json.dumps(license_manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
