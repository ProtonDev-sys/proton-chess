#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "move.h"

namespace proton {

struct UndoState {
    Piece captured = Empty;
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
    [[nodiscard]] const std::vector<Move>& move_history() const { return move_history_; }

    [[nodiscard]] Piece piece_at(int square) const { return board_[square]; }
    [[nodiscard]] Bitboard occupancy(Color color) const { return occupancy_[color]; }
    [[nodiscard]] Bitboard occupancy_all() const { return occupancy_[White] | occupancy_[Black]; }

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
    [[nodiscard]] bool is_draw_by_material() const;
    [[nodiscard]] bool is_repetition() const;
    [[nodiscard]] int king_square(Color color) const;

    std::uint64_t perft(int depth);
    void reset_history();

private:
    std::array<Piece, 64> board_{};
    std::array<Bitboard, 13> pieces_{};
    std::array<Bitboard, 2> occupancy_{};
    Color stm_ = White;
    int castling_ = 0;
    int ep_ = NoSquare;
    int halfmove_ = 0;
    int fullmove_ = 1;
    std::uint64_t key_ = 0;
    std::vector<std::uint64_t> history_keys_{};
    std::vector<Move> move_history_{};

    void clear();
    void recompute_occupancy();
    void rebuild_bitboards();
    void add_piece(Piece piece, int square);
    void remove_piece(Piece piece, int square);
    void move_piece(Piece piece, int from, int to);
    std::uint64_t compute_key() const;
    void push_history();
    void pop_history();
    void generate_pseudo_legal_moves(std::vector<Move>& moves, bool captures_only) const;
    bool make_move_unchecked(const Move& move, UndoState& undo);
};

}  // namespace proton
