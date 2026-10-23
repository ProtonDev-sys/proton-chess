"""Audit reproducible artifacts, game/position leakage and reference weights."""
import argparse
import collections
import hashlib
import importlib.metadata
import json
import pathlib

import numpy as np


def read_jsonl(path):
    return [json.loads(line) for line in path.read_text().splitlines()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('results/human-research'))
    parser.add_argument('--reference-model', type=pathlib.Path)
    args = parser.parse_args()
    out = args.out_dir.resolve()
    games = read_jsonl(out / 'games.jsonl')
    positions = read_jsonl(out / 'model-positions.jsonl')
    fitted = json.loads((out / 'linear-prior.json').read_text())
    extraction = json.loads((out / 'extraction.json').read_text())
    feature_manifest = json.loads((out / 'features.json').read_text())
    arrays = np.load(out / 'features.npz')
    by_split = {split: [row for row in positions if row['split'] == split] for split in ('train', 'valid', 'test')}
    ids = {split: {row['game_id'] for row in rows} for split, rows in by_split.items()}
    duplicate_games = len(games) - len({row['game_id'] for row in games})
    duplicate_decisions = len(positions) - len({(row['game_id'], row['ply']) for row in positions})
    game_overlap = {f'{a}_{b}': len(ids[a] & ids[b]) for a, b in (('train', 'valid'), ('train', 'test'), ('valid', 'test'))}
    previous = by_split['train'] + by_split['valid']
    full_fens = {row['fen'] for row in previous}
    state = lambda row: ' '.join(row['fen'].split()[:4])
    states = {state(row) for row in previous}
    full_overlap = sum(row['fen'] in full_fens for row in by_split['test'])
    state_overlap = sum(state(row) in states for row in by_split['test'])
    assert duplicate_games == duplicate_decisions == 0 and not any(game_overlap.values())
    assert all(row['ply'] > 12 and row['recorded_clock_before'] >= 30 and row['clock_after_before_increment'] >= 30
               and 1800 <= row['white_elo'] <= 2200 and 1800 <= row['black_elo'] <= 2200 for row in positions)
    assert len(positions) == feature_manifest['positions'] == len(arrays['targets'])
    assert len(arrays['x']) == feature_manifest['candidate_count'] == int(arrays['offsets'][-1])
    assert len(fitted['raw_feature_weights']) == len(fitted['feature_names']) == 53
    assert hashlib.sha256((out / 'model-positions.jsonl').read_bytes()).hexdigest() == fitted['positions_sha256'] == feature_manifest['model_positions_sha256']
    for name, digest in extraction['file_sha256'].items():
        assert hashlib.sha256((out / name).read_bytes()).hexdigest() == digest, name
    reference_error = None
    if args.reference_model:
        reference = json.loads(args.reference_model.read_text())
        assert fitted['feature_names'] == reference['feature_names']
        assert fitted['positions_sha256'] == reference['positions_sha256']
        reference_error = float(np.max(np.abs(np.asarray(fitted['raw_feature_weights']) - reference['raw_feature_weights'])))
        assert reference_error < 1e-9, reference_error
    result = {'status': 'pass', 'selected_games': len(games), 'model_positions': len(positions),
              'model_games': len({row['game_id'] for row in positions}), 'candidate_count': len(arrays['x']),
              'split_games': {split: len(value) for split, value in ids.items()},
              'split_positions': {split: len(value) for split, value in by_split.items()},
              'duplicate_selected_game_ids': duplicate_games, 'duplicate_model_game_ply_pairs': duplicate_decisions,
              'cross_split_game_id_overlap': game_overlap, 'test_full_fen_seen_in_train_or_valid': full_overlap,
              'test_fen_first4fields_seen_in_train_or_valid': state_overlap,
              'fen_note': 'First4fields preserve placement, turn, castling, EP; drop move/halfmove counters. Overlap is measured, not silently removed.',
              'max_reference_weight_error': reference_error,
              'package_versions': {name: importlib.metadata.version(name) for name in ('chess', 'numpy', 'scipy', 'zstandard')}}
    (out / 'pipeline-audit.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
