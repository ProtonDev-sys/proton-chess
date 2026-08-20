#include "see.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstddef>

#include "attacks.h"

namespace proton {
namespace {

constexpr std::size_t PieceSlotCount = 13;
using PieceBitboards = std::array<Bitboard, PieceSlotCount>;

[[nodiscard]] constexpr bool is_board_piece(Piece piece) {
    return piece >= WhitePawn && piece <= BlackKing;
}

[[nodiscard]] constexpr std::size_t piece_index(Piece piece) {
    const std::size_t index = static_cast<std::size_t>(piece);
    return index < PieceSlotCount ? index : 0;
}

[[nodiscard]] Bitboard colour_pieces(const PieceBitboards& pieces,
                                     Color color) {
    Bitboard result = 0;
    for (int type = Pawn; type <= King; ++type) {
        result |= pieces[piece_index(
            make_piece(color, static_cast<PieceType>(type)))];
    }
    return result;
}

struct LeastValuableAttacker {
    Piece piece = Empty;
    int square = NoSquare;
};

[[nodiscard]] bool aligned_with_king(int source, int king) {
    const int file_delta = std::abs(file_of(source) - file_of(king));
    const int rank_delta = std::abs(rank_of(source) - rank_of(king));
    return file_delta == 0 || rank_delta == 0 || file_delta == rank_delta;
}

[[nodiscard]] bool legal_recapture(int target, int source, Piece attacker,
                                      Bitboard occupied, Bitboard enemy,
                                      PieceBitboards& pieces,
                                      const std::array<int, 2>& king_squares,
                                      Color color) {
    if (!is_board_piece(attacker) || source < 0 || source >= 64) return false;
    const int king = piece_type(attacker) == King ? target : king_squares[color];
    if (king == NoSquare) return false;

    // A non-king move can expose its king only when the source lies on one of
    // the king's slider rays. Most SEE attackers therefore need no attack scan.
    if (piece_type(attacker) != King && !aligned_with_king(source, king)) return true;

    const Bitboard source_bit = bit(source);
    pieces[piece_index(attacker)] &= ~source_bit;
    const Bitboard occupied_after = occupied & ~source_bit;
    const bool legal =
        (attacks::attackers_to(king, occupied_after, pieces) & enemy) == 0;
    pieces[piece_index(attacker)] |= source_bit;
    return legal;
}

[[nodiscard]] LeastValuableAttacker least_valuable_attacker(
    Bitboard attackers, Bitboard occupied, PieceBitboards& pieces,
    const std::array<int, 2>& king_squares, Color color, int target) {
    const int king = king_squares[color];
    const Bitboard enemy = colour_pieces(pieces, opposite(color));
    if (king == NoSquare) return {};
    const bool in_discovered_check =
        (attacks::attackers_to(king, occupied, pieces) & enemy) != 0;

    // A legal king capture ends the exchange because the destination is not
    // attacked. It is therefore never worse than capturing the same target with
    // another piece and allowing the opponent the option to recapture.
    const Piece king_piece = make_piece(color, King);
    LeastValuableAttacker safe_king;
    Bitboard king_candidates = attackers & pieces[piece_index(king_piece)];
    while (king_candidates != 0) {
        const int square = static_cast<int>(std::countr_zero(king_candidates));
        king_candidates &= king_candidates - 1;
        if (legal_recapture(target, square, king_piece, occupied, enemy, pieces,
                            king_squares, color)) {
            safe_king = {king_piece, square};
            break;
        }
    }

    if (safe_king.piece != Empty) {
        // A safe promotion can dominate the king capture: it wins the same
        // target while also gaining the promotion value. Prefer it only when
        // removing the pawn does not uncover an enemy recapture; otherwise the
        // safe king capture remains the guaranteed best response.
        const bool promotion_rank = rank_of(target) == (color == White ? 7 : 0);
        if (promotion_rank) {
            const Piece pawn = make_piece(color, Pawn);
            Bitboard candidates = attackers & pieces[piece_index(pawn)];
            while (candidates != 0) {
                const int square = static_cast<int>(std::countr_zero(candidates));
                candidates &= candidates - 1;
                if (!legal_recapture(target, square, pawn, occupied, enemy, pieces,
                                     king_squares, color)) {
                    continue;
                }

                const Bitboard source_bit = bit(square);
                pieces[piece_index(pawn)] &= ~source_bit;
                const Bitboard occupied_after = occupied & ~source_bit;
                const bool can_be_recaptured =
                    (attacks::attackers_to(target, occupied_after, pieces) & enemy) != 0;
                pieces[piece_index(pawn)] |= source_bit;
                if (!can_be_recaptured) return {pawn, square};
            }
        }
        return safe_king;
    }

    if (in_discovered_check) return {};
    for (int type = Pawn; type <= Queen; ++type) {
        const Piece piece = make_piece(color, static_cast<PieceType>(type));
        Bitboard candidates = attackers & pieces[piece_index(piece)];
        while (candidates != 0) {
            const int square = static_cast<int>(std::countr_zero(candidates));
            candidates &= candidates - 1;
            if (legal_recapture(target, square, piece, occupied, enemy, pieces,
                                king_squares, color)) {
                return {piece, square};
            }
        }
    }
    return {};
}

}  // namespace

int static_exchange_eval(const Position& position, const Move& move) {
    if (move.is_null() || move.from >= 64 || move.to >= 64) return 0;

    const Color us = position.side_to_move();
    const Color them = opposite(us);
    const Piece moving = position.piece_at(move.from);
    if (!is_board_piece(moving) || piece_color(moving) != us) {
        return -piece_value(King);
    }
    if (move.is_promotion() &&
        (move.promotion < Knight || move.promotion > Queen)) {
        return -piece_value(King);
    }

    const int captured_square = (move.flags & MoveEnPassant) != 0
        ? move.to + (us == White ? -8 : 8)
        : move.to;
    Piece captured = attacks::on_board(captured_square)
        ? position.piece_at(captured_square)
        : Empty;
    if ((move.flags & MoveEnPassant) != 0) captured = make_piece(them, Pawn);

    if (captured == Empty && !move.is_promotion()) return 0;

    PieceBitboards pieces{};
    for (int piece = WhitePawn; piece <= BlackKing; ++piece) {
        pieces[static_cast<std::size_t>(piece)] =
            position.pieces(static_cast<Piece>(piece));
    }

    Bitboard occupied = position.occupancy_all();
    pieces[piece_index(moving)] &= ~bit(move.from);
    occupied &= ~bit(move.from);
    if (captured != Empty && attacks::on_board(captured_square)) {
        pieces[piece_index(captured)] &= ~bit(captured_square);
        occupied &= ~bit(captured_square);
    }
    occupied |= bit(move.to);

    const Piece placed = move.is_promotion() ? make_piece(us, move.promotion) : moving;
    PieceType target_type = piece_type(placed);
    std::array<int, 2> king_squares = {
        position.king_square(White), position.king_square(Black)};
    if (piece_type(moving) == King) king_squares[us] = move.to;

    // SEE is called before normal legality filtering. Reject pinned moves and
    // illegal king captures here so they cannot distort capture ordering or
    // tactical pruning.
    const int own_king = king_squares[us];
    const Bitboard enemy_after = colour_pieces(pieces, them);
    if (own_king == NoSquare ||
        (attacks::attackers_to(own_king, occupied, pieces) & enemy_after) != 0) {
        return -piece_value(King);
    }

    std::array<int, 32> gains{};
    gains[0] = piece_value(piece_type(captured));
    if (move.is_promotion()) {
        gains[0] += piece_value(move.promotion) - piece_value(Pawn);
    }

    Color side = them;
    int depth = 0;
    while (depth + 1 < static_cast<int>(gains.size())) {
        const Bitboard attackers = attacks::attackers_to(move.to, occupied, pieces) &
                                   colour_pieces(pieces, side);
        const LeastValuableAttacker attacker =
            least_valuable_attacker(attackers, occupied, pieces,
                                    king_squares, side, move.to);
        if (!is_board_piece(attacker.piece) || attacker.square < 0 ||
            attacker.square >= 64) {
            break;
        }

        const PieceType attacker_type = piece_type(attacker.piece);
        pieces[piece_index(attacker.piece)] &= ~bit(attacker.square);
        occupied &= ~bit(attacker.square);
        if (attacker_type == King) king_squares[side] = move.to;

        ++depth;
        gains[depth] = piece_value(target_type) - gains[depth - 1];

        if (attacker_type == Pawn &&
            rank_of(move.to) == (side == White ? 7 : 0)) {
            gains[depth] += piece_value(Queen) - piece_value(Pawn);
            target_type = Queen;
        } else {
            target_type = attacker_type;
        }
        side = opposite(side);
    }

    while (depth > 0) {
        gains[depth - 1] = -std::max(-gains[depth - 1], gains[depth]);
        --depth;
    }
    return gains[0];
}

}  // namespace proton
