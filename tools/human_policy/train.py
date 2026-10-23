"""Conditional logistic choice model, game-held-out validation/test."""
import os
os.environ['OPENBLAS_NUM_THREADS'] = '1'
os.environ['OMP_NUM_THREADS'] = '1'
import collections
import argparse
import hashlib
import json
import pathlib
import subprocess
import time

import numpy as np
from scipy.optimize import minimize, minimize_scalar

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/human-research'


def pack(x, offsets, targets, indexes):
    lengths = np.diff(offsets)[indexes]
    starts = np.r_[0, np.cumsum(lengths)[:-1]]
    rows = np.concatenate([np.arange(offsets[i], offsets[i + 1]) for i in indexes])
    chosen = starts + targets[indexes] - offsets[indexes]
    groups = np.repeat(np.arange(len(indexes)), lengths)
    return x[rows], starts, chosen, groups


def losses(scores, starts, targets, groups):
    maximum = np.maximum.reduceat(scores, starts)
    exps = np.exp(scores - maximum[groups])
    total = np.add.reduceat(exps, starts)
    nll = np.log(total) + maximum - scores[targets]
    tied = np.abs(scores - maximum[groups]) <= 1e-9
    top1 = tied[targets] / np.add.reduceat(tied.astype(float), starts)
    return nll, top1


def learn(x, starts, targets, groups, l2, initial=None):
    n = len(starts)
    def objective(w):
        scores = x @ w
        maximum = np.maximum.reduceat(scores, starts)
        exps = np.exp(scores - maximum[groups])
        sums = np.add.reduceat(exps, starts)
        value = np.mean(np.log(sums) + maximum - scores[targets]) + 0.5 * l2 * np.dot(w, w)
        probabilities = exps / sums[groups]
        probabilities[targets] -= 1
        gradient = x.T @ probabilities / n + l2 * w
        return value, gradient
    return minimize(objective, np.zeros(x.shape[1]) if initial is None else initial,
                    method='L-BFGS-B', jac=True,
                    options={'maxiter': 180, 'ftol': 1e-9, 'gtol': 1e-6})


def calibrate(scores, starts, targets, groups):
    result = minimize_scalar(lambda log_alpha: float(losses(scores * np.exp(log_alpha), starts, targets, groups)[0].mean()),
                             bounds=(-9, 3), method='bounded')
    return float(np.exp(result.x))


def summary(nll, top1):
    return {'nll_nats': float(np.mean(nll)), 'top1_tie_averaged': float(np.mean(top1)), 'positions': len(nll)}


def bootstrap_delta(learned, baseline, rows):
    ids = sorted(set(row['game_id'] for row in rows))
    lookup = {game: i for i, game in enumerate(ids)}
    groups = np.asarray([lookup[row['game_id']] for row in rows])
    counts = np.bincount(groups)
    deltas = np.vstack([np.bincount(groups, weights=learned[i] - baseline[i]) for i in range(2)])
    rng = np.random.default_rng(20261023)
    draws = rng.integers(0, len(ids), (2000, len(ids)))
    stats = deltas[:, draws].sum(axis=2) / counts[draws].sum(axis=1)[None, :]
    return {name: {'difference': float((learned[i] - baseline[i]).mean()),
                   'game_bootstrap_95ci': [float(v) for v in np.quantile(stats[i], [0.025, 0.975])]}
            for i, name in enumerate(('nll_nats', 'top1'))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('results/human-research'))
    parser.add_argument('--handcrafted-binary', type=pathlib.Path, help='Optional FEN-lines in, one JSON object per FEN out; see README.')
    parser.add_argument('--model-out', type=pathlib.Path, help='Optional copy of fitted weights JSON.')
    args = parser.parse_args()
    global OUT
    OUT = args.out_dir.resolve()
    started = time.time()
    artifact = np.load(OUT / 'features.npz')
    x, offsets, targets, splits = [artifact[k] for k in ('x', 'offsets', 'targets', 'splits')]
    indexes = [np.flatnonzero(splits == i) for i in range(3)]
    items = [json.loads(line) for line in (OUT / 'model-positions.jsonl').read_text().splitlines()]
    feature_names = json.loads((OUT / 'features.json').read_text())['feature_names']
    train, starts, chosen, groups = pack(x, offsets, targets, indexes[0])
    scale = np.maximum(np.std(train, axis=0), 0.05).astype(np.float64)
    train = train.astype(np.float64) / scale
    valid, vs, vt, vg = pack(x, offsets, targets, indexes[1])
    valid = valid.astype(np.float64) / scale
    fits = []
    weights = None
    for l2 in (0.01, 0.001, 0.0001):
        fit = learn(train, starts, chosen, groups, l2, weights)
        weights = fit.x
        valid_nll, valid_top1 = losses(valid @ weights, vs, vt, vg)
        entry = {'l2': l2, 'validation': summary(valid_nll, valid_top1),
                 'train': summary(*losses(train @ weights, starts, chosen, groups)),
                 'optimizer_success': bool(fit.success), 'optimizer_message': str(fit.message),
                 'iterations': int(fit.nit), 'weights_standardized': weights.tolist()}
        fits.append(entry)
        print(json.dumps({k: v for k, v in entry.items() if k != 'weights_standardized'}), flush=True)
    best = min(fits, key=lambda row: row['validation']['nll_nats'])
    raw_weights = np.asarray(best['weights_standardized']) / scale
    model = {'model': 'linear conditional logit / softmax over legal moves', 'feature_names': feature_names,
             'raw_feature_weights': raw_weights.tolist(), 'l2': best['l2'], 'training_positions': len(indexes[0]),
             'validation_positions': len(indexes[1]), 'weight_bytes_float32': len(raw_weights) * 4,
             'inference': 'logit(move)=sum(raw_feature_weights[j]*feature[j]); softmax only over legal candidate moves',
             'training_data': 'model-positions.jsonl (relative to --out-dir)',
             'positions_sha256': hashlib.sha256((OUT / 'model-positions.jsonl').read_bytes()).hexdigest(),
             'feature_definition': 'tools/human_policy/features.py',
             'claim': 'Human-game move-frequency prior; no strength, human indistinguishability, or 100% human claim.'}
    (OUT / 'linear-prior.json').write_text(json.dumps(model, indent=2))
    # Score genuine handmade source snapshot, preserving FEN and legal-list alignment.
    candidate_scores = {'uniform': np.zeros(len(x)), 'captures_checks': artifact['simple'].astype(float),
                        'learned_53features': x @ raw_weights}
    if args.handcrafted_binary:
        scorer = args.handcrafted_binary.resolve()
        with (OUT / 'model-fens.txt').open() as inp, (OUT / 'handcrafted-scores.jsonl').open('w') as out:
            subprocess.run([str(scorer)], stdin=inp, stdout=out, check=True)
        moves = json.loads((OUT / 'candidate-moves.json').read_text())
        policy = []
        lines = (OUT / 'handcrafted-scores.jsonl').read_text().splitlines()
        assert len(lines) == len(items), 'Scorer must return exactly one response per FEN'
        for i, line in enumerate(lines):
            row = json.loads(line)
            assert row['fen'] == items[i]['fen'], (i, 'FEN mismatch')
            scored = {item['uci']: item['score'] for item in row['moves']}
            assert set(scored) == set(moves[i]), (i, 'legal candidate mismatch')
            policy.extend(scored[move] for move in moves[i])
        assert len(policy) == len(x)
        candidate_scores['handcrafted'] = np.asarray(policy)
    calibrated = {}
    results = {}
    per_position = {}
    for name, scores in candidate_scores.items():
        vscore, _, _, _ = pack(scores, offsets, targets, indexes[1])
        alpha = 1 if name in ('uniform', 'learned_53features') else calibrate(vscore, vs, vt, vg)
        calibrated[name] = alpha
        split_results = {}
        for split, ids in zip(('train', 'valid', 'test'), indexes):
            packed, s, t, g = pack(scores, offsets, targets, ids)
            loss = losses(packed * alpha, s, t, g)
            split_results[split] = summary(*loss)
            per_position[(name, split)] = loss
        results[name] = split_results
    test_items = [items[i] for i in indexes[2]]
    train_ids = {items[i]['game_id'] for i in indexes[0]}
    valid_ids = {items[i]['game_id'] for i in indexes[1]}
    test_ids = {row['game_id'] for row in test_items}
    assert not (train_ids & valid_ids or train_ids & test_ids or valid_ids & test_ids)
    state = lambda row: ' '.join(row['fen'].split()[:4])
    previous_states = {state(items[i]) for i in np.r_[indexes[0], indexes[1]]}
    novel = np.asarray([state(row) not in previous_states for row in test_items])
    audit = {'games': {'train': len(train_ids), 'valid': len(valid_ids), 'test': len(test_ids)},
             'game_id_overlap': 0, 'test_positions_seen_in_train_or_valid': int((~novel).sum()),
             'test_novel_positions': int(novel.sum()), 'test_subgroups': {}}
    train_players = {player for line in (OUT / 'games.jsonl').read_text().splitlines()
                     for row in [json.loads(line)] if row['split'] == 'train' and row['game_id'] in train_ids
                     for player in (row['headers'].get('White'), row['headers'].get('Black'))}
    for label, mask in [('novel_positions', novel),
                        ('plies_over24', np.asarray([row['ply'] > 24 for row in test_items])),
                        ('rapid', np.asarray([row['event'] == 'Rated Rapid game' for row in test_items])),
                        ('classical', np.asarray([row['event'] == 'Rated Classical game' for row in test_items]))]:
        audit['test_subgroups'][label] = {name: summary(loss[0][mask], loss[1][mask])
                                           for (name, split), loss in per_position.items() if split == 'test' and mask.any()}
    game_rows = [json.loads(line) for line in (OUT / 'games.jsonl').read_text().splitlines()]
    test_game_rows = [row for row in game_rows if row['game_id'] in test_ids]
    audit['test_games_with_any_training_player'] = sum(any(row['headers'].get(key) in train_players for key in ('White', 'Black')) for row in test_game_rows)
    audit['sample_utc_dates'] = dict(collections.Counter(row['headers'].get('UTCDate') for row in game_rows))
    deltas = {name: bootstrap_delta(per_position[('learned_53features', 'test')], per_position[(name, 'test')], test_items)
              for name in ('uniform', 'captures_checks', 'handcrafted') if name in candidate_scores}
    report = {'results': results, 'validation_calibration_alpha': calibrated, 'hyperparameter_fits': fits,
              'selected_l2': best['l2'], 'audit': audit, 'test_paired_learned_minus_baseline': deltas,
              'runtime_seconds': time.time() - started,
              'top1_definition': 'Expected match rate with uniform random tie-breaking among maximal-logit legal moves.',
              'baseline_definition': 'captures_checks logit = victim(pawns) -0.1*mover(pawns)*capture +2*check +promotion_gain; positive scale fitted on valid only',
              'handcrafted_definition': 'genuine C++ Evaluator::policy_scores standalone, search/book/bounded selector excluded; positive scale fitted on valid only',
              'scope': 'All legal moves, game heldout; no engine strength or subjective human-likeness evaluation.'}
    (OUT / 'fit-results.json').write_text(json.dumps(report, indent=2))
    per_test = []
    for j, item in enumerate(test_items):
        per_test.append({'game_id': item['game_id'], 'ply': item['ply'],
                         **{name: {'nll': float(loss[0][j]), 'top1': float(loss[1][j])}
                            for (name, split), loss in per_position.items() if split == 'test'}})
    (OUT / 'heldout-per-position.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in per_test))
    if args.model_out:
        args.model_out.parent.mkdir(parents=True, exist_ok=True)
        args.model_out.write_text(json.dumps(model, indent=2))
    print(json.dumps({'results': results, 'selected_l2': best['l2'], 'audit': audit,
                      'deltas': deltas, 'runtime_seconds': time.time() - started}, indent=2), flush=True)


if __name__ == '__main__':
    main()
