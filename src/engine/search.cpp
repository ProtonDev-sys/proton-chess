#include "search.h"

#include <algorithm>
#include <fstream>
#include <numeric>
#include <sstream>

namespace proton {

namespace {

constexpr int NullMoveReduction = 2;
constexpr int DeltaMargin = 120;

struct OpeningBookLine {
    int weight = 1;
    std::vector<std::string> moves;
};

std::vector<OpeningBookLine> parse_book_lines(std::istream& input) {
    std::vector<OpeningBookLine> lines;
    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) {
            line.erase(comment);
        }

        OpeningBookLine entry;
        const std::size_t separator = line.find('|');
        if (separator != std::string::npos) {
            std::istringstream weight_stream(line.substr(0, separator));
            int weight = 1;
            if (weight_stream >> weight) {
                entry.weight = std::max(weight, 1);
            }
            line.erase(0, separator + 1);
        }

        std::istringstream stream(line);
        std::string move;
        while (stream >> move) {
            entry.moves.push_back(move);
        }
        if (!entry.moves.empty()) {
            lines.push_back(std::move(entry));
        }
    }
    return lines;
}

const std::vector<OpeningBookLine>& sequence_book() {
    static const std::vector<OpeningBookLine> book = [] {
        constexpr const char* BuiltinBook = R"BOOK(
e2e4 e7e5 g1f3 b8c6 f1c4 g8f6 d2d3
e2e4 e7e5 g1f3 b8c6 f1c4 f8c5 c2c3 g8f6 d2d4
e2e4 e7e5 b1c3 b8c6 g1f3 g8f6 f1b5
e2e4 e7e5 b1c3 b8c6 f1b5 g8f6 g1f3
e2e4 e7e5 f1c4 g8f6 d1e2 b8c6 c2c3 d7d5 e4d5 f6d5 d2d4 f8e7
e2e4 e7e6 d2d4 d7d5 b1c3 g8f6
e2e4 e7e6 b1c3 d7d5 d2d4 g8f6
e2e4 c7c5 g1f3 d7d6 d2d4 c5d4 f3d4 g8f6 b1c3 a7a6
e2e4 c7c5 g1f3 b8c6 d2d4 c5d4 f3d4 g7g6
e2e4 c7c6 d2d4 d7d5 b1c3
e2e4 d7d5 e4d5 d8d5 b1c3 d5a5 d2d4 g7g6 g1f3 g8f6
e2e4 d7d6 d2d4 g8f6 b1c3
d2d4 d7d5 c2c4 e7e6 b1c3 g8f6
d2d4 d7d5 c1f4 g8f6 e2e3
g1f3 d7d5 d2d4 g8f6 e2e3 c7c5
e2e3 d7d5 d2d4 g8f6 c2c4 e7e6 b1c3
e2e3 d7d5 c2c4 e7e6 c4d5 e6d5
c2c4 e7e5 b1c3 g8f6 d2d4 e5d4 d1d4 b8c6 d4d2 f8b4 g2g3 e8g8
)BOOK";

        for (const char* path : {"openings/book_lines.txt", "../openings/book_lines.txt", "../../openings/book_lines.txt"}) {
            std::ifstream file(path);
            if (!file) {
                continue;
            }
            std::vector<OpeningBookLine> parsed = parse_book_lines(file);
            if (!parsed.empty()) {
                return parsed;
            }
        }

        std::istringstream builtin(BuiltinBook);
        return parse_book_lines(builtin);
    }();
    return book;
}

Move sequence_book_move(const Position& position) {
    const auto& history = position.move_history();
    struct Candidate {
        Move move = Move::null();
        int weight = 0;
        std::size_t best_length = 0;
    };

    std::vector<Candidate> candidates;

    for (const OpeningBookLine& line : sequence_book()) {
        if (line.moves.size() <= history.size()) {
            continue;
        }

        bool matches = true;
        for (std::size_t index = 0; index < history.size(); ++index) {
            if (history[index].to_uci() != line.moves[index]) {
                matches = false;
                break;
            }
        }
        if (!matches) {
            continue;
        }

        const Move candidate = position.parse_uci_move(line.moves[history.size()]);
        if (candidate.is_null()) {
            continue;
        }

        auto it = std::find_if(candidates.begin(), candidates.end(), [&](const Candidate& item) {
            return item.move == candidate;
        });
        if (it == candidates.end()) {
            Candidate item;
            item.move = candidate;
            item.weight = line.weight;
            item.best_length = line.moves.size();
            candidates.push_back(item);
        } else {
            it->weight += line.weight;
            it->best_length = std::max(it->best_length, line.moves.size());
        }
    }

    Move best = Move::null();
    int best_weight = -1;
    std::size_t best_length = 0;
    for (const Candidate& candidate : candidates) {
        if (candidate.weight > best_weight ||
            (candidate.weight == best_weight && candidate.best_length > best_length)) {
            best = candidate.move;
            best_weight = candidate.weight;
            best_length = candidate.best_length;
        }
    }

    return best;
}

std::string opening_key(const std::string& fen) {
    std::istringstream stream(fen);
    std::string board;
    std::string stm;
    std::string castling;
    std::string ep;
    stream >> board >> stm >> castling >> ep;
    return board + " " + stm + " " + castling + " " + ep;
}

Move book_move(const Position& position) {
    if (Move move = sequence_book_move(position); !move.is_null()) {
        return move;
    }
    if (!sequence_book().empty()) {
        return Move::null();
    }

    static const std::pair<const char*, const char*> book[] = {
        {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq -", "e2e4"},
        {"rnbqkbnr/pppppppp/8/8/3P4/8/PPP1PPPP/RNBQKBNR b KQkq d3", "d7d5"},
        {"rnbqkbnr/pppppppp/8/8/8/4P3/PPPP1PPP/RNBQKBNR b KQkq -", "d7d5"},
        {"rnbqkbnr/pppppppp/8/8/8/2N5/PPPPPPPP/R1BQKBNR b KQkq -", "d7d5"},
        {"rnbqkbnr/pppppppp/8/8/1P6/8/P1PPPPPP/RNBQKBNR b KQkq b3", "e7e5"},
        {"rnbqkbnr/pppppppp/8/8/8/2P5/PP1PPPPP/RNBQKBNR b KQkq -", "d7d5"},
        {"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq e3", "e7e5"},
        {"rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6", "g1f3"},
        {"rnbqkbnr/pp1ppppp/2p5/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq -", "d2d4"},
        {"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq -", "e7e5"},
        {"rnbqkbnr/ppp1pppp/3p4/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq -", "d2d4"},
        {"rnbqkbnr/ppp1pppp/8/3p4/1P6/4P3/P1PP1PPP/RNBQKBNR b KQkq -", "e7e5"},
        {"rnbqkbnr/pppppppp/8/8/3PP3/8/PPP2PPP/RNBQKBNR b KQkq d3", "d7d5"},
        {"rnbqkbnr/pppppppp/8/8/2P5/8/PP1PPPPP/RNBQKBNR b KQkq c3", "e7e5"},
        {"rnbqkbnr/pppppppp/8/8/5N2/8/PPPPPPPP/RNBQKB1R b KQkq -", "d7d5"},
        {"rnbqkbnr/pp2pppp/2p5/3p4/3PP3/8/PPP2PPP/RNBQKBNR w KQkq d6", "b1c3"},
        {"rnbqkbnr/ppp2ppp/4p3/3p4/4P3/2N5/PPPP1PPP/R1BQKBNR w KQkq -", "d2d4"},
        {"rnbqkbnr/pppp1ppp/8/4p3/4P3/2N5/PPPP1PPP/R1BQKBNR b KQkq -", "g8f6"},
        {"rnbqkbnr/ppp2ppp/4p3/3p4/3PP3/8/PPP2PPP/RNBQKBNR w KQkq d6", "b1c3"},
        {"rnbqkbnr/pppp1ppp/4p3/8/4P3/8/PPPP1PPP/RNBQKBNR w KQkq -", "d2d4"},
        {"rnbqkb1r/pppp1ppp/5n2/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "d2d3"},
        {"rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "d2d4"},
        {"r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "d2d4"},
        {"rnbqkbnr/ppp1pppp/8/3p4/3PP3/8/PPP2PPP/RNBQKBNR w KQkq d6", "c2c4"},
        {"rnbqkbnr/pppp1ppp/8/4p3/2P5/8/PP1PPPPP/RNBQKBNR w KQkq e6", "b1c3"},
        {"r1bqkbnr/ppp2ppp/2n1p3/3p4/4P3/2N2N2/PPPP1PPP/R1BQKB1R w KQkq d6", "f1b5"},
        {"r1bqkbnr/pp1ppppp/2n5/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "f1b5"},
        {"rnbqkbnr/pp2pppp/3p4/2p5/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "f1b5"},
        {"r1bqkbnr/pppp1ppp/2n1p3/8/4P3/2N5/PPPP1PPP/R1BQKBNR w KQkq -", "d2d4"},
        {"r1bqkb1r/ppp1nppp/2n1p3/3p4/3PP3/2N2N2/PPP2PPP/R1BQKB1R w KQkq -", "f1d3"},
        {"rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq -", "b8c6"},
        {"r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq -", "f1c4"},
        {"r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R b KQkq -", "f8c5"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq -", "d2d3"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2NP1/PPPP1P1P/R1BQKB1R b KQkq -", "f8c5"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/4P3/2N2N1P/PPPP1PP1/R1BQKB1R b KQkq -", "f8c5"},
        {"r1bqk2r/ppppbppp/2n2n2/4p3/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq -", "d2d3"},
        {"rnbqkbnr/3p1ppp/pp2p3/2p5/3PP3/2N2N2/PPP2PPP/R1BQKB1R w KQkq -", "f1d3"},
        {"rnbqkbnr/3p1ppp/pp2p3/8/3pP3/2NB1N2/PPP2PPP/R1BQK2R w KQkq -", "f3d4"},
        {"r1bqkbnr/3p1ppp/ppn1p3/8/3NP3/2NB4/PPP2PPP/R1BQK2R w KQkq -", "c1e3"},
        {"rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b KQkq c3", "e7e6"},
        {"rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/8/PP2PPPP/RNBQKBNR w KQkq -", "b1c3"},
        {"rnbqkbnr/ppp2ppp/4p3/3p4/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq -", "g8f6"},
        {"rnbqkbnr/pp2pppp/3p4/2p5/3PP3/5N2/PPP2PPP/RNBQKB1R b KQkq -", "c5d4"},
        {"rnbqkbnr/pp2pppp/3p4/8/3NP3/8/PPP2PPP/RNBQKB1R b KQkq -", "g8f6"},
        {"rnbqkb1r/pp2pppp/3p1n2/8/3NP3/2N5/PPP2PPP/R1BQKB1R b KQkq -", "a7a6"},
        {"rnbqkb1r/pppp1ppp/5n2/4p3/2PP4/2N5/PP2PPPP/R1BQKBNR b KQkq d3", "e5d4"},
        {"rnbqkb1r/pppp1ppp/5n2/8/2PQ4/2N5/PP2PPPP/R1B1KBNR b KQkq -", "b8c6"},
        {"r1bqkb1r/pppp1ppp/2n2n2/8/2P5/2N5/PP1QPPPP/R1B1KBNR b KQkq -", "f8b4"},
        {"r1bqk2r/pppp1ppp/2n2n2/8/1bP5/2N3P1/PP1QPP1P/R1B1KBNR b KQkq -", "e8g8"},
        {"r1bqkbnr/pppp1ppp/2n5/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR b KQkq -", "g8f6"},
        {"r1bqkb1r/pppp1ppp/2n2n2/4p3/2P5/2N5/PP1PPPPP/R1BQKBNR w KQkq -", "g1f3"},
        {"r1bqkb1r/pp1ppp1p/2n2np1/2p5/2B1P3/2N2N2/PPPP1PPP/R1BQK2R w KQkq -", "d2d4"},
        {"rnb1kbnr/ppp1pppp/8/q7/8/2N5/PPPP1PPP/R1BQKBNR w KQkq -", "d2d4"},
        {"rnb1kbnr/ppp1pp1p/6p1/q7/3P4/2N5/PPP2PPP/R1BQKBNR w KQkq -", "g1f3"},
        {"rnb1kb1r/ppp1pp1p/5np1/q7/3P4/2N2N2/PPP2PPP/R1BQKB1R w KQkq -", "f1c4"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/3P4/5N2/PPPNPPPP/R1BQKB1R b KQkq -", "e7e6"},
        {"rnbqkbnr/ppp1pppp/8/3p4/3P4/4P3/PPP2PPP/RNBQKBNR b KQkq d3", "g8f6"},
        {"rnbqkbnr/ppp1pppp/8/3p4/2P5/4P3/PP1P1PPP/RNBQKBNR b KQkq -", "e7e6"},
        {"rnbqkbnr/ppp2ppp/4p3/3P4/8/4P3/PP1P1PPP/RNBQKBNR b KQkq -", "e6d5"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/3P1P2/4P3/PPP3PP/RNBQKBNR b KQkq f3", "c7c5"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/3P4/4PN2/PPP2PPP/RNBQKB1R b KQkq -", "c7c5"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/3P4/4PN2/PPP2PPP/RNBQKB1R b KQkq -", "e7e6"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/2PP4/4P3/PP3PPP/RNBQKBNR b KQkq -", "e7e6"},
        {"rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/5N2/PP1NPPPP/R1BQKB1R b KQkq c3", "c7c5"},
        {"rnbqkb1r/ppp2ppp/4pn2/3p4/2PP4/2N1P3/PP3PPP/R1BQKBNR b KQkq -", "f8e7"},
        {"rnbqkb1r/ppp1pppp/5n2/3p4/3P1B2/5N2/PPP1PPPP/RN1QKB1R b KQkq -", "e7e6"},
        {"rnbqkbnr/ppp1pppp/8/3p4/3P1B2/8/PPP1PPPP/RN1QKBNR b KQkq -", "g8f6"},
        {"rnbqkb1r/ppp2ppp/4pn2/3p4/2PP1B2/5N2/PP2PPPP/RN1QKB1R b KQkq c3", "c7c5"},
        {"r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/2N5/PPPP1PPP/R1BQK1NR b KQkq -", "g8f6"},
        {"r1bqkbnr/pppp1ppp/2n5/1B2p3/4P3/5N2/PPPP1PPP/RNBQK2R b KQkq -", "a7a6"},
        {"r1bqkb1r/ppp2ppp/2n5/3np3/2BP4/2P5/PP2QPPP/RNB1K1NR b KQkq -", "f8e7"},
        {"rnbqkb1r/ppp2ppp/4pn2/3p4/3P4/4PN2/PPP2PPP/RNBQKB1R w KQkq -", "c2c4"},
    };

    const std::string fen = opening_key(position.fen());
    for (const auto& [key, move_text] : book) {
        if (fen == key) {
            return position.parse_uci_move(move_text);
        }
    }
    return Move::null();
}

int mvv_lva(const Position& position, const Move& move) {
    if (!move.is_capture()) {
        return 0;
    }
    const Piece victim = position.piece_at(move.to);
    const Piece attacker = position.piece_at(move.from);
    return 10 * piece_value(piece_type(victim)) - piece_value(piece_type(attacker));
}

int capture_value(const Position& position, const Move& move) {
    if (!move.is_capture()) {
        return 0;
    }
    Piece victim = position.piece_at(move.to);
    if ((move.flags & MoveEnPassant) != 0) {
        victim = position.side_to_move() == White ? BlackPawn : WhitePawn;
    }
    int value = piece_value(piece_type(victim));
    if (move.is_promotion()) {
        value += piece_value(move.promotion) - piece_value(Pawn);
    }
    return value;
}

int quiet_bonus(const Position& position, const Move& move) {
    const Piece piece = position.piece_at(move.from);
    int bonus = 0;

    if ((move.flags & MoveKingCastle) != 0 || (move.flags & MoveQueenCastle) != 0) {
        bonus += 80;
    }
    if (move.is_promotion()) {
        bonus += 120 + piece_value(move.promotion);
    }

    const int to_file = file_of(move.to);
    const int to_rank = rank_of(move.to);
    bonus += 4 - (std::abs(to_file - 3) + std::abs(to_rank - 3));

    if (piece == WhiteKnight && (move.from == 1 || move.from == 6)) {
        bonus += 18;
    } else if (piece == BlackKnight && (move.from == 57 || move.from == 62)) {
        bonus += 18;
    } else if (piece == WhiteBishop && (move.from == 2 || move.from == 5)) {
        bonus += 14;
    } else if (piece == BlackBishop && (move.from == 58 || move.from == 61)) {
        bonus += 14;
    }

    return bonus;
}

}  // namespace

Search::Search() {
    resize_table(options_.hash_mb);
    clear_heuristics();
}

void Search::resize_table(int hash_mb) {
    const std::size_t bytes = static_cast<std::size_t>(std::max(hash_mb, 1)) * 1024ULL * 1024ULL;
    const std::size_t count = std::max<std::size_t>(1, bytes / sizeof(TTEntry));
    table_.assign(count, TTEntry{});
}

void Search::clear_heuristics() {
    for (auto& ply_killers : killers_) {
        ply_killers = {Move::null(), Move::null()};
    }
    for (auto& color_hist : history_) {
        for (auto& from_hist : color_hist) {
            from_hist.fill(0);
        }
    }
}

void Search::set_options(const EngineOptions& options) {
    options_ = options;
    resize_table(options.hash_mb);
}

bool Search::time_expired() const {
    return !stop_ && deadline_ != std::chrono::steady_clock::time_point{} &&
           std::chrono::steady_clock::now() >= deadline_;
}

const Search::TTEntry* Search::probe_tt(std::uint64_t key) const {
    if (table_.empty()) {
        return nullptr;
    }
    const TTEntry& entry = table_[key % table_.size()];
    return entry.key == key ? &entry : nullptr;
}

void Search::store_tt(std::uint64_t key, int depth, int score, Bound bound, const Move& best) {
    if (table_.empty()) {
        return;
    }
    TTEntry& entry = table_[key % table_.size()];
    if (entry.key != key || depth >= entry.depth) {
        entry.key = key;
        entry.depth = depth;
        entry.score = score;
        entry.best = best;
        entry.bound = bound;
    }
}

int Search::score_move(const Position& position, const Move& move, const Move& tt_move, int ply) const {
    if (move == tt_move) {
        return 1'000'000;
    }
    if (move.is_capture()) {
        return 500'000 + mvv_lva(position, move) + capture_value(position, move);
    }
    if (ply < MaxPly) {
        if (move == killers_[ply][0]) {
            return 400'000;
        }
        if (move == killers_[ply][1]) {
            return 399'000;
        }
    }
    return history_[position.side_to_move()][move.from][move.to] + quiet_bonus(position, move);
}

int Search::quiescence(Position& position, int alpha, int beta, int ply) {
    ++nodes_;
    if ((nodes_ & 2047ULL) == 0 && time_expired()) {
        stop_ = true;
        return alpha;
    }

    const bool in_check = position.in_check(position.side_to_move());
    int stand_pat = -Infinity;
    if (!in_check) {
        stand_pat = evaluator_->evaluate(position);
        if (stand_pat >= beta) {
            return beta;
        }
        alpha = std::max(alpha, stand_pat);
    }

    std::vector<Move> moves;
    if (in_check) {
        position.generate_pseudo_moves(moves);
    } else {
        position.generate_pseudo_captures(moves);
    }
    std::sort(moves.begin(), moves.end(), [&](const Move& lhs, const Move& rhs) {
        return score_move(position, lhs, Move::null(), ply) > score_move(position, rhs, Move::null(), ply);
    });

    for (const Move& move : moves) {
        if (!in_check && !move.is_promotion() && stand_pat + capture_value(position, move) + DeltaMargin < alpha) {
            continue;
        }
        UndoState undo;
        if (!position.make_move(move, undo)) {
            continue;
        }
        const int score = -quiescence(position, -beta, -alpha, ply + 1);
        position.unmake_move(move, undo);

        if (stop_) {
            return alpha;
        }
        if (score >= beta) {
            return beta;
        }
        alpha = std::max(alpha, score);
    }

    return alpha;
}

int Search::negamax(Position& position, int depth, int alpha, int beta, int ply, bool allow_null) {
    ++nodes_;
    if ((nodes_ & 2047ULL) == 0 && time_expired()) {
        stop_ = true;
        return alpha;
    }

    if (ply >= MaxPly - 1) {
        return evaluator_->evaluate(position);
    }

    if (position.is_repetition() || position.halfmove_clock() >= 100 || position.is_draw_by_material()) {
        return 0;
    }

    const bool in_check = position.in_check(position.side_to_move());
    if (depth <= 0) {
        if (!in_check) {
            return quiescence(position, alpha, beta, ply);
        }
        depth = 1;
    } else if (in_check) {
        ++depth;
    }

    const int original_alpha = alpha;
    Move tt_move = Move::null();
    if (const TTEntry* entry = probe_tt(position.key())) {
        tt_move = entry->best;
        if (entry->depth >= depth) {
            if (entry->bound == BoundExact) {
                return entry->score;
            }
            if (entry->bound == BoundLower && entry->score >= beta) {
                return entry->score;
            }
            if (entry->bound == BoundUpper && entry->score <= alpha) {
                return entry->score;
            }
        }
    }

    if (allow_null && depth >= 3 && !in_check) {
        UndoState undo;
        position.make_null_move(undo);
        const int score = -negamax(position, depth - 1 - NullMoveReduction, -beta, -beta + 1, ply + 1, false);
        position.unmake_null_move(undo);
        if (stop_) {
            return alpha;
        }
        if (score >= beta) {
            return beta;
        }
    }

    std::vector<Move> moves;
    position.generate_pseudo_moves(moves);
    if (moves.empty()) {
        return in_check ? -MateScore + ply : 0;
    }

    std::sort(moves.begin(), moves.end(), [&](const Move& lhs, const Move& rhs) {
        return score_move(position, lhs, tt_move, ply) > score_move(position, rhs, tt_move, ply);
    });

    Move best_move = Move::null();
    int best_score = -Infinity;
    bool found_move = false;

    for (std::size_t index = 0; index < moves.size(); ++index) {
        const Move& move = moves[index];
        UndoState undo;
        if (!position.make_move(move, undo)) {
            continue;
        }

        int score = 0;
        const bool quiet = !move.is_capture() && !move.is_promotion();
        const bool late_reduction = depth >= 3 && index >= 3 && quiet && !in_check;
        const int full_depth = depth - 1;

        if (late_reduction) {
            score = -negamax(position, full_depth - 1, -alpha - 1, -alpha, ply + 1, true);
            if (score > alpha) {
                if (index == 0) {
                    score = -negamax(position, full_depth, -beta, -alpha, ply + 1, true);
                } else {
                    score = -negamax(position, full_depth, -alpha - 1, -alpha, ply + 1, true);
                    if (score > alpha && score < beta) {
                        score = -negamax(position, full_depth, -beta, -alpha, ply + 1, true);
                    }
                }
            }
        } else {
            if (index == 0) {
                score = -negamax(position, full_depth, -beta, -alpha, ply + 1, true);
            } else {
                score = -negamax(position, full_depth, -alpha - 1, -alpha, ply + 1, true);
                if (score > alpha && score < beta) {
                    score = -negamax(position, full_depth, -beta, -alpha, ply + 1, true);
                }
            }
        }

        position.unmake_move(move, undo);
        if (stop_) {
            return alpha;
        }

        found_move = true;
        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            if (quiet && ply < MaxPly) {
                history_[position.side_to_move()][move.from][move.to] += depth * depth;
            }
        }
        if (alpha >= beta) {
            if (quiet && ply < MaxPly) {
                killers_[ply][1] = killers_[ply][0];
                killers_[ply][0] = move;
            }
            break;
        }
    }

    if (!found_move) {
        return in_check ? -MateScore + ply : 0;
    }

    Bound bound = BoundExact;
    if (best_score <= original_alpha) {
        bound = BoundUpper;
    } else if (best_score >= beta) {
        bound = BoundLower;
    }
    store_tt(position.key(), depth, best_score, bound, best_move);
    return best_score;
}

SearchResult Search::find_best_move(Position& position, const SearchLimits& limits, Evaluator& evaluator) {
    evaluator_ = &evaluator;
    stop_ = false;
    nodes_ = 0;

    if (Move move = book_move(position); !move.is_null()) {
        SearchResult result;
        result.best_move = move;
        return result;
    }

    if (limits.movetime_ms > 0) {
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(limits.movetime_ms);
    } else if (!limits.infinite && ((position.side_to_move() == White && limits.wtime_ms > 0) ||
                                    (position.side_to_move() == Black && limits.btime_ms > 0))) {
        const int clock = position.side_to_move() == White ? limits.wtime_ms : limits.btime_ms;
        const int inc = position.side_to_move() == White ? limits.winc_ms : limits.binc_ms;
        const int budget = std::max(25, clock / 20 + inc / 2);
        deadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget);
    } else {
        deadline_ = std::chrono::steady_clock::time_point{};
    }

    std::vector<Move> root_moves;
    position.generate_pseudo_moves(root_moves);
    SearchResult result{};
    if (root_moves.empty()) {
        return result;
    }

    std::vector<float> policy = evaluator.policy_scores(position, root_moves);
    std::vector<std::size_t> order(root_moves.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const float lhs_score = policy[lhs] * 100.0F + static_cast<float>(score_move(position, root_moves[lhs], Move::null(), 0));
        const float rhs_score = policy[rhs] * 100.0F + static_cast<float>(score_move(position, root_moves[rhs], Move::null(), 0));
        return lhs_score > rhs_score;
    });

    std::vector<Move> ordered_moves;
    ordered_moves.reserve(root_moves.size());
    for (std::size_t idx : order) {
        ordered_moves.push_back(root_moves[idx]);
    }
    root_moves = ordered_moves;

    int previous_score = 0;
    for (int depth = 1; depth <= limits.depth; ++depth) {
        int window = depth >= 3 ? 30 : Infinity;
        int alpha = depth >= 3 ? previous_score - window : -Infinity;
        int beta = depth >= 3 ? previous_score + window : Infinity;

        while (true) {
            int best_score = -Infinity;
            Move best_move = Move::null();
            int local_alpha = alpha;
            bool searched_root = false;
            bool found_legal = false;

            for (const Move& move : root_moves) {
                UndoState undo;
                if (!position.make_move(move, undo)) {
                    continue;
                }
                int score = 0;
                if (!searched_root) {
                    score = -negamax(position, depth - 1, -beta, -local_alpha, 1, true);
                    searched_root = true;
                } else {
                    score = -negamax(position, depth - 1, -local_alpha - 1, -local_alpha, 1, true);
                    if (score > local_alpha && score < beta) {
                        score = -negamax(position, depth - 1, -beta, -local_alpha, 1, true);
                    }
                }
                position.unmake_move(move, undo);

                if (!found_legal || score > best_score) {
                    best_score = score;
                    best_move = move;
                    found_legal = true;
                }
                if (stop_) {
                    break;
                }
                local_alpha = std::max(local_alpha, score);
            }

            if (stop_) {
                break;
            }
            if (!found_legal) {
                break;
            }
            if (best_score <= alpha && alpha != -Infinity) {
                alpha -= 50;
                beta = best_score + 20;
                continue;
            }
            if (best_score >= beta && beta != Infinity) {
                beta += 50;
                alpha = best_score - 20;
                continue;
            }

            previous_score = best_score;
            result.best_move = best_move;
            result.score_cp = best_score;
            result.depth = depth;
            result.nodes = nodes_;
            auto it = std::find(root_moves.begin(), root_moves.end(), best_move);
            if (it != root_moves.end()) {
                std::rotate(root_moves.begin(), it, it + 1);
            }
            break;
        }

        if (stop_) {
            break;
        }
    }

    if (result.best_move.is_null()) {
        std::vector<Move> legal_moves;
        position.generate_legal_moves(legal_moves);
        if (!legal_moves.empty()) {
            result.best_move = legal_moves.front();
        }
        result.nodes = nodes_;
    }
    return result;
}

}  // namespace proton
