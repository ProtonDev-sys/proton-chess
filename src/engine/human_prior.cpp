#include "human_prior.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cmath>
#include <utility>

#include "attacks.h"

namespace proton {
namespace {

// human-club-v1, 2026-10-23. Exact raw_feature_weights / feature order from
// models/human-club-v1.json; feature definitions: tools/human_policy/features.py
// (original experiment: build/human-research/features.py). Training used 18,731
// human-game positions, validation 4,137; test 4,133, held out by game.
// Data: September 2026 Lichess standard rated database (CC0-1.0), archive prefix;
// source URL, cohort filters and sampling limitations are recorded in the model.
// Model positions SHA256:
// 35c81b67eba8cc4ebb9c332d996658b16bb3142e5f4b1a4c0a5d892a2013880b
// Original results/human-research/linear-prior.json SHA256:
// 7f9958c6bb4f2f8689e0261eea05ad8e6c5d1109232e9189cf2367e99aac0a90
// This is a move-frequency prior, not a strength or human-likeness guarantee.
constexpr std::array<double, 53> Weights = {
    -0.1536760237591934,     // moving_pawn
    -0.47697680240950463,    // moving_knight
     0.4066512004488427,     // moving_bishop
    -0.06050781698824106,    // moving_rook
     0.2874376691244849,     // moving_queen
    -0.09700994635582413,    // moving_king
     1.3273108898857822,     // capture
     0.7400954735653803,     // captured_value
     0.011195713475650933,   // capture_mover_cost
     0.3607816283815592,     // capture_if_attacked_net
     0.053158771588038244,   // promotion_gain
     1.8899657249733446,     // gives_check
     3.082773123246481,      // castle
     0.7472300068771037,     // source_attacked
    -0.34661987029040464,    // destination_attacked
     0.06668989091760417,    // destination_defended
    -0.9041183170165019,     // destination_enemy_attackers
     0.07380363082680756,    // destination_defenders
    -0.22862031919858933,    // mover_value_if_attacked
    -0.5439849264603334,     // mover_value_if_undefended_attacked
     0.5873275315929406,     // escape_attack
    -1.2243384481141044,     // capture_safe
    -1.6978735757225873,     // quiet_safe
     0.015534364494595373,   // centrality_delta
     0.060118752794680264,   // destination_centrality
     0.11401936292108969,    // minor_centrality_delta
     0.04799696069600478,    // queen_centrality_delta
    -0.09335459006470777,    // king_middlegame_centrality_delta
     0.34752097296308393,    // king_endgame_centrality_delta
     1.1793980611544246,     // minor_home_development
    -0.17666732522579773,    // minor_repeat_while_undeveloped
    -0.2210192819471074,     // queen_while_undeveloped
    -0.08923687164180032,    // pawn_advance
     0.5586397432520208,     // pawn_centre_file
    -0.07503421544872785,    // pawn_edge_file
    -0.07695451942449169,    // pawn_double
     1.0399145915041121,     // rook_open_file_delta
     0.8946849859845971,     // rook_semiopen_file_delta
     0.6880049745396876,     // rook_seventh_rank
     0.060412985528902945,   // passed_pawn_advance
     0.5203110623318992,     // passed_pawn_endgame_advance
     0.14399560629946842,    // new_attacks_enemy_value
     0.07274584059368087,    // attacks_undefended_enemy_value
    -0.3231394097675671,     // new_attacks_enemy_queen
     0.5073722138204692,     // king_distance_delta_endgame
     0.03056971705597347,    // capture_when_material_ahead
     0.0812593954208154,     // capture_when_material_behind
     0.08349862746917298,    // pawn_promotion_distance
    -0.05769763625882845,    // bishop_diagonal_activity
     0.11385524690043476,    // knight_activity
    -0.08556223832047935,    // quiet_check_evasion
     0.2268689330876081,     // capture_check_evasion
    -0.09700994635609853,    // safe_king_move
};

// Training units are pawns, including bishop=3.3 and king=0. These intentionally
// differ from the engine's evaluation/SEE values.
constexpr std::array<double, 6> Values = {1.0, 3.2, 3.3, 5.0, 9.0, 0.0};

double value(Piece piece) {
    return piece == Empty ? 0.0 : Values[piece_type(piece)];
}

int relative_rank(int square, Color color) {
    return color == White ? rank_of(square) : 7 - rank_of(square);
}

int centrality(int square) {
    return 7 - (std::abs(2 * file_of(square) - 7) +
                std::abs(2 * rank_of(square) - 7)) / 2;
}

int distance(int from, int to) {
    return std::max(std::abs(file_of(from) - file_of(to)),
                    std::abs(rank_of(from) - rank_of(to)));
}

Bitboard attacks_from(const Position& board, int square) {
    const Piece piece = board.piece_at(square);
    const Bitboard occupied = board.occupancy_all();
    switch (piece_type(piece)) {
    case Pawn: return attacks::pawn(piece_color(piece), square);
    case Knight: return attacks::Knight[square];
    case Bishop: return attacks::bishop(square, occupied);
    case Rook: return attacks::rook(square, occupied);
    case Queen: return attacks::queen(square, occupied);
    case King: return attacks::King[square];
    default: return 0;
    }
}

// Geometric attacks deliberately include pinned pieces, occupied friendly
// destinations and king attacks, exactly as python-chess Board.attackers().
Bitboard attackers(const Position& board, int square, Color color) {
    const Bitboard occupied = board.occupancy_all();
    const Bitboard queens = board.pieces(make_piece(color, Queen));
    return (attacks::pawn(opposite(color), square) & board.pieces(make_piece(color, Pawn))) |
           (attacks::Knight[square] & board.pieces(make_piece(color, Knight))) |
           (attacks::bishop(square, occupied) & (board.pieces(make_piece(color, Bishop)) | queens)) |
           (attacks::rook(square, occupied) & (board.pieces(make_piece(color, Rook)) | queens)) |
           (attacks::King[square] & board.pieces(make_piece(color, King)));
}

bool home_minor(const Position& board, int square, Color color) {
    if (rank_of(square) != (color == White ? 0 : 7)) return false;
    const int file = file_of(square);
    const PieceType expected = file == 1 || file == 6 ? Knight
        : file == 2 || file == 5 ? Bishop : NoPieceType;
    return expected != NoPieceType && board.piece_at(square) == make_piece(color, expected);
}

std::pair<int, int> file_status(const Position& board, Color color, int file) {
    const Bitboard mask = attacks::FileA << file;
    const bool own = (board.pieces(make_piece(color, Pawn)) & mask) != 0;
    const bool enemy = (board.pieces(make_piece(opposite(color), Pawn)) & mask) != 0;
    return {!own && !enemy, !own && enemy};
}

bool passed(const Position& board, int square, Color color) {
    const int file = file_of(square);
    const int rank = relative_rank(square, color);
    Bitboard pawns = board.pieces(make_piece(opposite(color), Pawn));
    while (pawns != 0) {
        const int pawn = static_cast<int>(std::countr_zero(pawns));
        pawns &= pawns - 1;
        if (std::abs(file_of(pawn) - file) <= 1 && relative_rank(pawn, color) > rank) return false;
    }
    pawns = board.pieces(make_piece(color, Pawn));
    while (pawns != 0) {
        const int pawn = static_cast<int>(std::countr_zero(pawns));
        pawns &= pawns - 1;
        if (file_of(pawn) == file && relative_rank(pawn, color) > rank) return false;
    }
    return true;
}

}  // namespace

std::vector<float> human_move_logits(const Position& position, const std::vector<Move>& moves) {
    std::vector<float> logits;
    if (moves.empty()) return logits;
    logits.reserve(moves.size());
    const Color color = position.side_to_move();
    const Color enemy = opposite(color);
    double nonpawn = 0.0;
    for (const PieceType type : {Knight, Bishop, Rook, Queen}) {
        for (const Color side : {White, Black}) {
            nonpawn += Values[type] * std::popcount(position.pieces(make_piece(side, type)));
        }
    }
    const bool endgame = (position.pieces(WhiteQueen) | position.pieces(BlackQueen)) == 0 && nonpawn <= 22.0;
    bool undeveloped = false;
    for (const int file : {1, 6, 2, 5}) {
        undeveloped = undeveloped || home_minor(position, (color == White ? 0 : 56) + file, color);
    }
    const bool development = undeveloped && nonpawn > 22.0;
    double material = 0.0;
    for (const PieceType type : {Pawn, Knight, Bishop, Rook, Queen}) {
        material += Values[type] * (std::popcount(position.pieces(make_piece(color, type))) -
                                   std::popcount(position.pieces(make_piece(enemy, type))));
    }
    const bool evading = position.in_check(color);
    const int enemy_king = position.king_square(enemy);
    Position board = position;
    for (const Move& move : moves) {
        const PieceType piece = piece_type(position.piece_at(move.from));
        const double mover_value = Values[piece];
        const bool capture = move.is_capture();
        const double victim = (move.flags & MoveEnPassant) != 0 ? 1.0 : value(position.piece_at(move.to));
        const double promotion = move.is_promotion() ? Values[move.promotion] - 1.0 : 0.0;
        const bool quiet = !capture && promotion == 0.0;
        const bool attacked_before = position.is_square_attacked(move.from, enemy);
        const int centre_to = centrality(move.to);
        const int delta = centre_to - centrality(move.from);
        const int pawn_step = piece == Pawn ? relative_rank(move.to, color) - relative_rank(move.from, color) : 0;
        const auto rook_before = piece == Rook ? file_status(position, color, file_of(move.from)) : std::pair{0, 0};
        const Bitboard old_targets = attacks_from(position, move.from) & position.occupancy(enemy);
        const bool developing_minor = home_minor(position, move.from, color);
        UndoState undo;
        const bool legal = board.make_move(move, undo);
        assert(legal && "human_move_logits requires legal candidates");
        if (!legal) return {};
        const bool check = board.in_check(enemy);
        const Bitboard enemy_attackers = attackers(board, move.to, enemy);
        const Bitboard own_defenders = attackers(board, move.to, color);
        const bool attacked = enemy_attackers != 0;
        const bool defended = own_defenders != 0;
        Bitboard targets = attacks_from(board, move.to) & board.occupancy(enemy);
        double new_value = 0.0;
        double undefended_value = 0.0;
        bool queen_target = false;
        while (targets != 0) {
            const int target = static_cast<int>(std::countr_zero(targets));
            targets &= targets - 1;
            if ((old_targets & bit(target)) == 0) {
                new_value += value(board.piece_at(target));
                queen_target = queen_target || piece_type(board.piece_at(target)) == Queen;
            }
            if (!board.is_square_attacked(target, enemy)) undefended_value += value(board.piece_at(target));
        }
        const auto rook_after = piece == Rook ? file_status(board, color, file_of(move.to)) : std::pair{0, 0};
        const bool passer = piece == Pawn && !move.is_promotion() && passed(board, move.to, color);
        const int activity = std::popcount(attacks_from(board, move.to) & ~board.occupancy(color));
        board.unmake_move(move, undo);

        // Python constructs features in double precision, then stores each row
        // as np.float32. Preserve that rounding before the double-weight dot.
        std::array<float, Weights.size()> features{};
        std::size_t index = 0;
        const auto add = [&](double feature) { features[index++] = static_cast<float>(feature); };
        for (const PieceType type : {Pawn, Knight, Bishop, Rook, Queen, King}) add(piece == type);
        add(capture);
        add(victim);
        add(mover_value * capture);
        add((victim - mover_value) * attacked * capture);
        add(promotion);
        add(check);
        add(move.is_castle());
        add(attacked_before);
        add(attacked);
        add(defended);
        add(std::popcount(enemy_attackers));
        add(std::popcount(own_defenders));
        add(mover_value * attacked);
        add(mover_value * attacked * !defended);
        add(attacked_before && !attacked);
        add(capture && !attacked);
        add(quiet && !attacked);
        add(delta);
        add(centre_to);
        add(delta * (piece == Knight || piece == Bishop));
        add(delta * (piece == Queen));
        add(delta * (piece == King) * !endgame);
        add(delta * (piece == King) * endgame);
        add(development && developing_minor);
        add(development && quiet && (piece == Knight || piece == Bishop) && !developing_minor);
        add(development && quiet && piece == Queen);
        add(pawn_step);
        add(piece == Pawn && (file_of(move.to) == 3 || file_of(move.to) == 4));
        add(piece == Pawn && (file_of(move.to) == 0 || file_of(move.to) == 7));
        add(piece == Pawn && pawn_step == 2);
        add(rook_after.first - rook_before.first);
        add(rook_after.second - rook_before.second);
        add(piece == Rook && relative_rank(move.to, color) == 6);
        add(pawn_step * passer);
        add(pawn_step * passer * endgame);
        add(new_value);
        add(undefended_value);
        add(queen_target);
        add(enemy_king == NoSquare ? 0 : (distance(move.from, enemy_king) - distance(move.to, enemy_king)) * (piece == King) * endgame);
        add(capture * std::max(0.0, material));
        add(capture * std::max(0.0, -material));
        add(relative_rank(move.to, color) * (piece == Pawn));
        add(activity * (piece == Bishop));
        add(activity * (piece == Knight));
        add(quiet && evading);
        add(capture && evading);
        add(piece == King && !attacked);
        assert(index == features.size());
        double logit = 0.0;
        for (std::size_t feature = 0; feature < features.size(); ++feature) {
            logit += Weights[feature] * static_cast<double>(features[feature]);
        }
        logits.push_back(static_cast<float>(logit));
    }
    return logits;
}

}  // namespace proton
