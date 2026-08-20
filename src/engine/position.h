#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "move.h"

namespace proton {

struct UndoState {
    Piece captured = Empty;
    std::int8_t ep_hash_file = -1;
    int captured_square = NoSquare;
    int castling_rights = 0;
    int ep_square = NoSquare;
    int halfmove_clock = 0;
    int fullmove_number = 1;
    std::uint64_t key = 0;
};

class Position {
public:
    Position();

    void set_startpos();
    bool set_fen(const std::string& fen);
    [[nodiscard]] std::string fen() const;

    [[nodiscard]] Color side_to_move() const { return stm_; }
    [[nodiscard]] int castling_rights() const { return castling_; }
    [[nodiscard]] int en_passant_square() const { return ep_; }
    [[nodiscard]] int halfmove_clock() const { return halfmove_; }
    [[nodiscard]] int fullmove_number() const { return fullmove_; }
    [[nodiscard]] std::uint64_t key() const { return key_; }
    [[nodiscard]] std::uint64_t pawn_key() const { return pawn_key_; }
    [[nodiscard]] const std::vector<Move>& move_history() const { return move_history_; }
    [[nodiscard]] Piece piece_at(int square) const { return board_[square]; }
    [[nodiscard]] Bitboard pieces(Piece piece) const { return pieces_[piece]; }
    [[nodiscard]] Bitboard occupancy(Color color) const { return occupancy_[color]; }
    [[nodiscard]] Bitboard occupancy_all() const { return occupancy_[White] | occupancy_[Black]; }
    [[nodiscard]] bool has_non_pawn_material(Color color) const;

    void generate_legal_moves(std::vector<Move>& moves) const;
    void generate_captures(std::vector<Move>& moves) const;
    void generate_pseudo_moves(std::vector<Move>& moves) const;
    void generate_pseudo_captures(std::vector<Move>& moves) const;
    [[nodiscard]] Move parse_uci_move(const std::string& text) const;

    bool make_move(const Move& move, UndoState& undo);
    void unmake_move(const Move& move, const UndoState& undo);
    void make_null_move(UndoState& undo);
    void unmake_null_move(const UndoState& undo);

    [[nodiscard]] bool is_square_attacked(int square, Color by) const;
    [[nodiscard]] bool in_check(Color color) const;
    // Predicts the opponent's check state after a structurally valid pseudo-move.
    // Own-king legality remains the responsibility of make_move().
    [[nodiscard]] bool gives_check(const Move& move) const;
    [[nodiscard]] bool is_draw_by_material() const;
    [[nodiscard]] bool is_repetition(int required_occurrences = 3) const;
    [[nodiscard]] int king_square(Color color) const;

    std::uint64_t perft(int depth);
    void reset_history();

private:
    std::array<Piece, 64> board_{};
    std::array<Bitboard, 13> pieces_{};
    std::array<Bitboard, 2> occupancy_{};
    Color stm_ = White;
    std::int8_t ep_hash_file_ = -1;
    int castling_ = 0;
    int ep_ = NoSquare;
    int halfmove_ = 0;
    int fullmove_ = 1;
    int null_depth_ = 0;
    std::vector<std::size_t> null_barriers_{};
    std::uint64_t key_ = 0;
    std::uint64_t pawn_key_ = 0;
    std::vector<std::uint64_t> history_keys_{};
    std::vector<Move> move_history_{};

    void clear();
    void rebuild_bitboards();
    void add_piece(Piece piece, int square);
    void remove_piece(Piece piece, int square);
    void move_piece(Piece piece, int from, int to);
    [[nodiscard]] std::uint64_t compute_key() const;
    [[nodiscard]] std::int8_t ep_hash_file() const;
    void push_history();
    void pop_history();
    void generate_pseudo_legal_moves(std::vector<Move>& moves, bool captures_only) const;
    bool make_move_unchecked(const Move& move, UndoState& undo);
};

}  // namespace proton
