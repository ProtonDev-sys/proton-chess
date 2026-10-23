"""Interpretable legal-move features, no search or engine evaluation."""
import collections
import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import time

os.environ.setdefault('OPENBLAS_NUM_THREADS', '1')
import chess
import numpy as np

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/human-research'
SEED = 'proton-human-prior-v1-20261023'
MAX_PER_GAME = 12
VALUES = [0, 1, 3.2, 3.3, 5, 9, 0]
NAMES = [f'moving_{name}' for name in ('pawn', 'knight', 'bishop', 'rook', 'queen', 'king')] + [
    'capture', 'captured_value', 'capture_mover_cost', 'capture_if_attacked_net', 'promotion_gain',
    'gives_check', 'castle', 'source_attacked', 'destination_attacked', 'destination_defended',
    'destination_enemy_attackers', 'destination_defenders', 'mover_value_if_attacked',
    'mover_value_if_undefended_attacked', 'escape_attack', 'capture_safe', 'quiet_safe',
    'centrality_delta', 'destination_centrality', 'minor_centrality_delta',
    'queen_centrality_delta', 'king_middlegame_centrality_delta', 'king_endgame_centrality_delta',
    'minor_home_development', 'minor_repeat_while_undeveloped', 'queen_while_undeveloped',
    'pawn_advance', 'pawn_centre_file', 'pawn_edge_file', 'pawn_double',
    'rook_open_file_delta', 'rook_semiopen_file_delta', 'rook_seventh_rank',
    'passed_pawn_advance', 'passed_pawn_endgame_advance',
    'new_attacks_enemy_value', 'attacks_undefended_enemy_value', 'new_attacks_enemy_queen',
    'king_distance_delta_endgame', 'capture_when_material_ahead', 'capture_when_material_behind',
    'pawn_promotion_distance', 'bishop_diagonal_activity', 'knight_activity',
    'quiet_check_evasion', 'capture_check_evasion', 'safe_king_move',
]


def centre(square):
    return 7 - (abs(2 * chess.square_file(square) - 7) + abs(2 * chess.square_rank(square) - 7)) / 2


def rank(square, color):
    return chess.square_rank(square) if color else 7 - chess.square_rank(square)


def file_status(board, color, file):
    own = bool(board.pieces_mask(chess.PAWN, color) & chess.BB_FILES[file])
    enemy = bool(board.pieces_mask(chess.PAWN, not color) & chess.BB_FILES[file])
    return (int(not own and not enemy), int(not own and enemy))


def passed(board, square, color):
    f, r = chess.square_file(square), rank(square, color)
    for pawn in board.pieces(chess.PAWN, not color):
        if abs(chess.square_file(pawn) - f) <= 1 and rank(pawn, color) > r:
            return False
    return not any(chess.square_file(pawn) == f and rank(pawn, color) > r
                   for pawn in board.pieces(chess.PAWN, color) if pawn != square)


def candidate_features(board, moves):
    color = board.turn
    enemy = not color
    nonpawn = sum(VALUES[p] * len(board.pieces(p, c)) for p in (2, 3, 4, 5) for c in (True, False))
    endgame = not board.queens and nonpawn <= 22
    home_rank = 0 if color else 7
    home = {chess.square(f, home_rank): p for f, p in ((1, 2), (6, 2), (2, 3), (5, 3))}
    undeveloped = sum(board.piece_type_at(sq) == p and board.color_at(sq) == color for sq, p in home.items())
    dev = undeveloped > 0 and nonpawn > 22
    material = sum(VALUES[p] * (len(board.pieces(p, color)) - len(board.pieces(p, enemy))) for p in range(1, 6))
    evading = board.is_check()
    enemy_king = board.king(enemy)
    rows, simple = [], []
    for move in moves:
        piece = board.piece_type_at(move.from_square)
        mover_value = VALUES[piece]
        capture = board.is_capture(move)
        victim = 1 if board.is_en_passant(move) else VALUES[board.piece_type_at(move.to_square) or 0]
        promotion = VALUES[move.promotion] - 1 if move.promotion else 0
        castle = board.is_castling(move)
        attacked_before = board.is_attacked_by(enemy, move.from_square)
        cf, ct = centre(move.from_square), centre(move.to_square)
        delta = ct - cf
        pawn_step = rank(move.to_square, color) - rank(move.from_square, color) if piece == 1 else 0
        rook_before = file_status(board, color, chess.square_file(move.from_square)) if piece == 4 else (0, 0)
        old_targets = board.attacks(move.from_square) & board.occupied_co[enemy]
        king_distance_before = chess.square_distance(move.from_square, enemy_king) if enemy_king is not None else 0
        board.push(move)
        check = board.is_check()
        attackers = board.attackers(enemy, move.to_square)
        defenders = board.attackers(color, move.to_square)
        attacked, defended = bool(attackers), bool(defenders)
        targets = board.attacks(move.to_square) & board.occupied_co[enemy]
        new_value = sum(VALUES[board.piece_type_at(sq)] for sq in targets if sq not in old_targets)
        undefended_value = sum(VALUES[board.piece_type_at(sq)] for sq in targets if not board.is_attacked_by(enemy, sq))
        queen_target = any(board.piece_type_at(sq) == 5 and sq not in old_targets for sq in targets)
        rook_after = file_status(board, color, chess.square_file(move.to_square)) if piece == 4 else (0, 0)
        passer = piece == 1 and not move.promotion and passed(board, move.to_square, color)
        activity = len(board.attacks(move.to_square) & ~board.occupied_co[color])
        board.pop()
        quiet = not capture and not promotion
        home_minor = home.get(move.from_square) == piece
        row = [int(piece == p) for p in range(1, 7)] + [
            capture, victim, mover_value * capture, (victim - mover_value) * attacked * capture, promotion,
            check, castle, attacked_before, attacked, defended, len(attackers), len(defenders),
            mover_value * attacked, mover_value * attacked * (not defended), attacked_before and not attacked,
            capture and not attacked, quiet and not attacked,
            delta, ct, delta * (piece in (2, 3)), delta * (piece == 5),
            delta * (piece == 6) * (not endgame), delta * (piece == 6) * endgame,
            dev and home_minor, dev and quiet and piece in (2, 3) and not home_minor,
            dev and quiet and piece == 5, pawn_step, piece == 1 and chess.square_file(move.to_square) in (3, 4),
            piece == 1 and chess.square_file(move.to_square) in (0, 7), piece == 1 and pawn_step == 2,
            rook_after[0] - rook_before[0], rook_after[1] - rook_before[1], piece == 4 and rank(move.to_square, color) == 6,
            pawn_step * passer, pawn_step * passer * endgame,
            new_value, undefended_value, queen_target,
            (king_distance_before - chess.square_distance(move.to_square, enemy_king)) * (piece == 6) * endgame if enemy_king is not None else 0,
            capture * max(0, material), capture * max(0, -material),
            rank(move.to_square, color) * (piece == 1), activity * (piece == 3), activity * (piece == 2),
            quiet and evading, capture and evading, piece == 6 and not attacked,
        ]
        assert len(row) == len(NAMES), (len(row), len(NAMES))
        rows.append(row)
        # Deliberately simple, fixed interpretable baseline, calibrated only on validation.
        simple.append(victim - 0.1 * mover_value * capture + 2 * check + promotion)
    return np.asarray(rows, dtype=np.float32), np.asarray(simple, dtype=np.float32)


def main():
    global OUT, SEED, MAX_PER_GAME
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('results/human-research'))
    parser.add_argument('--seed', default=SEED)
    parser.add_argument('--max-positions-per-game', type=int, default=12)
    args = parser.parse_args()
    OUT, SEED, MAX_PER_GAME = args.out_dir.resolve(), args.seed, args.max_positions_per_game
    if MAX_PER_GAME <= 0:
        parser.error('--max-positions-per-game must be positive')
    started = time.time()
    games = collections.defaultdict(list)
    for line in (OUT / 'positions.jsonl').read_text().splitlines():
        item = json.loads(line)
        games[item['game_id']].append(item)
    items = []
    for game_id, rows in games.items():
        rows.sort(key=lambda x: hashlib.sha256((SEED + ':' + game_id + ':' + str(x['ply'])).encode()).digest())
        items.extend(sorted(rows[:MAX_PER_GAME], key=lambda x: x['ply']))
    (OUT / 'model-positions.jsonl').write_text(''.join(json.dumps(item) + '\n' for item in items))
    (OUT / 'model-fens.txt').write_text(''.join(item['fen'] + '\n' for item in items))
    all_features, simple, offsets, targets, all_moves = [], [], [0], [], []
    for index, item in enumerate(items):
        board = chess.Board(item['fen'])
        moves = list(board.legal_moves)
        uci = [move.uci() for move in moves]
        assert item['move'] in uci
        target = uci.index(item['move'])
        x, baseline = candidate_features(board, moves)
        all_features.append(x)
        simple.append(baseline)
        targets.append(offsets[-1] + target)
        offsets.append(offsets[-1] + len(moves))
        all_moves.append(uci)
        if (index + 1) % 2000 == 0:
            print(f'features {index + 1}/{len(items)} {time.time() - started:.1f}s', flush=True)
    x = np.concatenate(all_features)
    np.savez_compressed(OUT / 'features.npz', x=x, simple=np.concatenate(simple),
                        offsets=np.asarray(offsets, dtype=np.int64), targets=np.asarray(targets, dtype=np.int64),
                        splits=np.asarray([{'train': 0, 'valid': 1, 'test': 2}[item['split']] for item in items], dtype=np.int8))
    (OUT / 'candidate-moves.json').write_text(json.dumps(all_moves))
    manifest = {'feature_names': NAMES, 'feature_count': len(NAMES), 'candidate_count': len(x),
                'positions': len(items), 'games': len(set(item['game_id'] for item in items)),
                'split_counts': dict(collections.Counter(item['split'] for item in items)),
                'max_positions_per_game': MAX_PER_GAME, 'elapsed_seconds': time.time() - started,
                'feature_caveats': 'Geometric attackers include pinned attackers; no SEE/search/evaluation. Features use current position and candidate only.',
                'model_positions_sha256': hashlib.sha256((OUT / 'model-positions.jsonl').read_bytes()).hexdigest()}
    (OUT / 'features.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
