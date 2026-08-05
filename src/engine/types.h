#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace proton {

using Bitboard = std::uint64_t;

enum Color : std::uint8_t { White = 0, Black = 1, NoColor = 2 };

constexpr Color opposite(Color color) {
    return color == White ? Black : White;
}

enum PieceType : std::uint8_t {
    Pawn = 0,
    Knight = 1,
    Bishop = 2,
    Rook = 3,
    Queen = 4,
    King = 5,
    NoPieceType = 255
};

enum Piece : std::uint8_t {
    Empty = 0,
    WhitePawn,
    WhiteKnight,
    WhiteBishop,
    WhiteRook,
    WhiteQueen,
    WhiteKing,
    BlackPawn,
    BlackKnight,
    BlackBishop,
    BlackRook,
    BlackQueen,
    BlackKing
};

constexpr int NoSquare = -1;

constexpr int file_of(int square) { return square & 7; }
constexpr int rank_of(int square) { return square >> 3; }
constexpr Bitboard bit(int square) { return 1ULL << square; }

constexpr Color piece_color(Piece piece) {
    if (piece >= WhitePawn && piece <= WhiteKing) return White;
    if (piece >= BlackPawn && piece <= BlackKing) return Black;
    return NoColor;
}

constexpr PieceType piece_type(Piece piece) {
    switch (piece) {
    case WhitePawn:
    case BlackPawn: return Pawn;
    case WhiteKnight:
    case BlackKnight: return Knight;
    case WhiteBishop:
    case BlackBishop: return Bishop;
    case WhiteRook:
    case BlackRook: return Rook;
    case WhiteQueen:
    case BlackQueen: return Queen;
    case WhiteKing:
    case BlackKing: return King;
    default: return NoPieceType;
    }
}

constexpr Piece make_piece(Color color, PieceType type) {
    if (type == NoPieceType || color == NoColor) return Empty;
    return static_cast<Piece>((color == White ? static_cast<int>(WhitePawn)
                                               : static_cast<int>(BlackPawn))
                              + static_cast<int>(type));
}

constexpr char piece_to_char(Piece piece) {
    constexpr std::array<char, 13> table = {
        '.', 'P', 'N', 'B', 'R', 'Q', 'K', 'p', 'n', 'b', 'r', 'q', 'k'};
    return table[static_cast<std::size_t>(piece)];
}

constexpr Piece char_to_piece(char c) {
    switch (c) {
    case 'P': return WhitePawn;
    case 'N': return WhiteKnight;
    case 'B': return WhiteBishop;
    case 'R': return WhiteRook;
    case 'Q': return WhiteQueen;
    case 'K': return WhiteKing;
    case 'p': return BlackPawn;
    case 'n': return BlackKnight;
    case 'b': return BlackBishop;
    case 'r': return BlackRook;
    case 'q': return BlackQueen;
    case 'k': return BlackKing;
    default: return Empty;
    }
}

inline std::string square_to_string(int square) {
    if (square < 0 || square >= 64) return "-";
    std::string out = "a1";
    out[0] = static_cast<char>('a' + file_of(square));
    out[1] = static_cast<char>('1' + rank_of(square));
    return out;
}

inline int square_from_string(const std::string& text) {
    if (text.size() != 2 || text[0] < 'a' || text[0] > 'h' ||
        text[1] < '1' || text[1] > '8') {
        return NoSquare;
    }
    return (text[1] - '1') * 8 + (text[0] - 'a');
}

constexpr int piece_value(PieceType type) {
    switch (type) {
    case Pawn: return 100;
    case Knight: return 320;
    case Bishop: return 335;
    case Rook: return 500;
    case Queen: return 950;
    case King: return 20000;
    default: return 0;
    }
}

}  // namespace proton
