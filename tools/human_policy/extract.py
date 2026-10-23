"""Stream partial zstd, retain only PGNs terminated by the next Event header."""
import collections
import argparse
import hashlib
import io
import json
import pathlib
import re
import time

import chess
import chess.pgn
import zstandard

ROOT = pathlib.Path(__file__).resolve().parents[2]
OUT = ROOT / 'results/human-research'
COUNTS = collections.Counter()
EVENTS = collections.Counter()
SEED = 'proton-human-prior-v1-20261023'
MAX_POSITIONS_PER_GAME = 40


def partition(game_id):
    bucket = int(hashlib.sha256((SEED + ':' + game_id).encode()).hexdigest()[:8], 16) % 100
    return 'train' if bucket < 70 else 'valid' if bucket < 85 else 'test'


def process(raw, pgn_out, games_out, positions_out):
    COUNTS['complete_games_seen'] += 1
    # Header parsing alone is cheap, so unselected movetext is never parsed.
    header = dict(re.findall(r'^\[(\w+) "(.*)"\]$', raw, re.M))
    event = header.get('Event', '')
    EVENTS[event] += 1
    if event not in ('Rated Rapid game', 'Rated Classical game'):
        return
    COUNTS['rated_rapid_or_classical'] += 1
    if header.get('Variant', 'Standard') != 'Standard' or 'FEN' in header:
        COUNTS['nonstandard_or_setup'] += 1
        return
    try:
        white, black = int(header['WhiteElo']), int(header['BlackElo'])
    except (KeyError, ValueError):
        COUNTS['missing_or_invalid_rating'] += 1
        return
    if not (1800 <= white <= 2200 and 1800 <= black <= 2200):
        return
    COUNTS['both_ratings_1800_2200'] += 1
    if header.get('WhiteTitle') == 'BOT' or header.get('BlackTitle') == 'BOT':
        COUNTS['bot_title_excluded'] += 1
        return
    try:
        base, increment = map(int, header['TimeControl'].split('+'))
    except (KeyError, ValueError):
        COUNTS['invalid_time_control'] += 1
        return
    if header.get('Result') not in ('1-0', '0-1', '1/2-1/2'):
        COUNTS['unfinished_result'] += 1
        return
    game = chess.pgn.read_game(io.StringIO(raw))
    if game is None or game.errors:
        COUNTS['pgn_parse_errors'] += 1
        return
    game_id = header.get('Site', hashlib.sha256(raw.encode()).hexdigest())
    split = partition(game_id)
    board = game.board()
    previous_clock = {chess.WHITE: float(base), chess.BLACK: float(base)}
    eligible = []
    for ply, node in enumerate(game.mainline(), 1):
        COUNTS['selected_game_total_plies'] += 1
        mover = board.turn
        before, after = previous_clock[mover], node.clock()
        reason = None
        if ply <= 12:
            reason = 'excluded_first_12_plies'
        elif before is None or after is None:
            reason = 'excluded_unknown_clock'
        elif min(before, after - increment) < 30:
            reason = 'excluded_clock_under_30'
        if reason:
            COUNTS[reason] += 1
        else:
            COUNTS['eligible_plies_before_per_game_cap'] += 1
            eligible.append({'game_id': game_id, 'split': split, 'ply': ply, 'fen': board.fen(),
                             'move': node.move.uci(), 'recorded_clock_before': before,
                             'recorded_clock_after': after, 'increment_seconds': increment,
                             'clock_after_before_increment': after - increment, 'white_elo': white, 'black_elo': black,
                             'event': event, 'time_control': header['TimeControl']})
        previous_clock[mover] = after
        board.push(node.move)
    # Fixed hash selection avoids the first-40-eligible-plies bias, using no target move.
    eligible.sort(key=lambda x: hashlib.sha256((SEED + ':' + game_id + ':' + str(x['ply'])).encode()).digest())
    kept = sorted(eligible[:MAX_POSITIONS_PER_GAME], key=lambda x: x['ply'])
    COUNTS['retained_games'] += 1
    COUNTS[f'games_{split}'] += 1
    COUNTS[f'event_{event}'] += 1
    COUNTS['retained_positions'] += len(kept)
    COUNTS[f'positions_{split}'] += len(kept)
    pgn_out.write(raw)
    if not raw.endswith('\n\n'):
        pgn_out.write('\n')
    games_out.write(json.dumps({'game_id': game_id, 'split': split, 'headers': header,
                               'eligible_positions': len(eligible), 'retained_positions': len(kept)}) + '\n')
    for item in kept:
        positions_out.write(json.dumps(item) + '\n')


def main():
    global OUT, SEED, MAX_POSITIONS_PER_GAME
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=pathlib.Path, default=pathlib.Path('results/human-research'))
    parser.add_argument('--archive', type=pathlib.Path, help='Default: OUT/prefix.pgn.zst')
    parser.add_argument('--expected-sha256', default='d629023c6ceb487afcd676cc840e744a1f97b8f8782e9f0af3f846998fa205f6')
    parser.add_argument('--seed', default=SEED)
    parser.add_argument('--max-positions-per-game', type=int, default=40)
    args = parser.parse_args()
    OUT, SEED, MAX_POSITIONS_PER_GAME = args.out_dir.resolve(), args.seed, args.max_positions_per_game
    OUT.mkdir(parents=True, exist_ok=True)
    archive = args.archive or OUT / 'prefix.pgn.zst'
    if args.max_positions_per_game <= 0:
        parser.error('--max-positions-per-game must be positive')
    if archive.stat().st_size > 32 * 1024 * 1024:
        parser.error('Refusing an archive larger than the research 32MiB cap')
    if args.expected_sha256 and hashlib.sha256(archive.read_bytes()).hexdigest() != args.expected_sha256:
        parser.error('Compressed prefix SHA-256 mismatch')
    started = time.time()
    uncompressed = 0
    current = []
    with archive.open('rb') as source, \
            zstandard.ZstdDecompressor().stream_reader(source) as decoder, \
            io.TextIOWrapper(decoder, encoding='utf-8') as text, \
            (OUT / 'selected.pgn').open('w') as pgn_out, \
            (OUT / 'games.jsonl').open('w') as games_out, \
            (OUT / 'positions.jsonl').open('w') as positions_out:
        for line in text:
            uncompressed += len(line.encode('utf-8'))
            if line.startswith('[Event ') and current:
                process(''.join(current), pgn_out, games_out, positions_out)
                current = []
            current.append(line)
    COUNTS['trailing_partial_game_discarded'] = int(bool(current))
    manifest = {'counts': dict(COUNTS), 'all_event_counts': dict(EVENTS),
                'decompressed_bytes': uncompressed, 'elapsed_seconds': time.time() - started,
                'split_seed': SEED, 'split_rule': 'sha256(seed:game_id) first32bits mod100: <70 train; <85 valid; else test',
                'max_positions_per_game': MAX_POSITIONS_PER_GAME,
                'position_selection': 'smallest sha256(seed:game_id:ply), excluding target move',
                'clock_filter': 'previous recorded mover clock (initially base) and post-move clock minus increment both known and >=30sec; conservative exclusion of increment-masked time pressure',
                'opening_filter': 'exclude plies1..12 inclusive; ply13 is first possible retained decision',
                'human_filter': 'rated rapid/classical Event, both ratings1800..2200 inclusive, BOT titles excluded; cannot certify no undisclosed engine assistance',
                'source_order_caveat': 'archive prefix is a contiguous early-month slice, not a random monthly sample',
                'file_sha256': {name: hashlib.sha256((OUT / name).read_bytes()).hexdigest()
                                for name in ('selected.pgn', 'games.jsonl', 'positions.jsonl')}}
    (OUT / 'extraction.json').write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2), flush=True)


if __name__ == '__main__':
    main()
