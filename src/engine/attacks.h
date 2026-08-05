#pragma once

#include <array>
#include <bit>

#include "types.h"

namespace proton::attacks {

inline constexpr std::array<int, 8> KnightOffsets = {17, 15, 10, 6, -17, -15, -10, -6};
inline constexpr std::array<int, 8> KingOffsets = {8, -8, 1, -1, 9, 7, -9, -7};
inline constexpr std::array<int, 4> BishopDirections = {9, 7, -9, -7};
inline constexpr std::array<int, 4> RookDirections = {8, -8, 1, -1};
inline constexpr std::array<int, 8> RayDirections = {8, -8, 1, -1, 9, 7, -7, -9};
inline constexpr Bitboard FileA = 0x0101010101010101ULL;
inline constexpr Bitboard FileH = 0x8080808080808080ULL;

[[nodiscard]] constexpr bool on_board(int square) {
    return square >= 0 && square < 64;
}

[[nodiscard]] constexpr int abs_int(int value) {
    return value < 0 ? -value : value;
}

[[nodiscard]] constexpr bool valid_step(int from, int to, int delta) {
    if (!on_board(to)) return false;
    const int file_delta = abs_int(file_of(from) - file_of(to));
    switch (abs_int(delta)) {
    case 1:
    case 7:
    case 9:
    case 15:
    case 17: return file_delta == 1;
    case 6:
    case 10: return file_delta == 2;
    default: return file_delta == 0;
    }
}

[[nodiscard]] consteval std::array<Bitboard, 64> make_leaper_table(
    const std::array<int, 8>& offsets) {
    std::array<Bitboard, 64> table{};
    for (int square = 0; square < 64; ++square) {
        for (const int delta : offsets) {
            const int target = square + delta;
            if (valid_step(square, target, delta)) table[square] |= bit(target);
        }
    }
    return table;
}

inline constexpr std::array<Bitboard, 64> Knight = make_leaper_table(KnightOffsets);
inline constexpr std::array<Bitboard, 64> King = make_leaper_table(KingOffsets);

[[nodiscard]] consteval std::array<std::array<Bitboard, 64>, 8> make_ray_table() {
    std::array<std::array<Bitboard, 64>, 8> table{};
    for (int direction = 0; direction < 8; ++direction) {
        const int delta = RayDirections[direction];
        for (int square = 0; square < 64; ++square) {
            int current = square;
            while (true) {
                const int target = current + delta;
                if (!valid_step(current, target, delta)) break;
                table[direction][square] |= bit(target);
                current = target;
            }
        }
    }
    return table;
}

inline constexpr std::array<std::array<Bitboard, 64>, 8> Rays = make_ray_table();

[[nodiscard]] constexpr Bitboard pawn_map(Bitboard pawns, Color color) {
    if (color == White) {
        return ((pawns & ~FileA) << 7U) | ((pawns & ~FileH) << 9U);
    }
    return ((pawns & ~FileA) >> 9U) | ((pawns & ~FileH) >> 7U);
}

[[nodiscard]] constexpr Bitboard pawn(Color color, int square) {
    return pawn_map(bit(square), color);
}

[[nodiscard]] inline Bitboard ray(int direction, int square, Bitboard occupied) {
    Bitboard result = Rays[direction][square];
    const Bitboard blockers = result & occupied;
    if (blockers == 0) return result;

    const bool increasing = RayDirections[direction] > 0;
    const int blocker = increasing
        ? static_cast<int>(std::countr_zero(blockers))
        : 63 - static_cast<int>(std::countl_zero(blockers));
    return result ^ Rays[direction][blocker];
}

[[nodiscard]] inline Bitboard bishop(int square, Bitboard occupied) {
    return ray(4, square, occupied) | ray(5, square, occupied) |
           ray(6, square, occupied) | ray(7, square, occupied);
}

[[nodiscard]] inline Bitboard rook(int square, Bitboard occupied) {
    return ray(0, square, occupied) | ray(1, square, occupied) |
           ray(2, square, occupied) | ray(3, square, occupied);
}

[[nodiscard]] inline Bitboard queen(int square, Bitboard occupied) {
    return bishop(square, occupied) | rook(square, occupied);
}

[[nodiscard]] inline Bitboard attackers_to(
    int square, Bitboard occupied, const std::array<Bitboard, 13>& pieces) {
    return (pawn(Black, square) & pieces[WhitePawn]) |
           (pawn(White, square) & pieces[BlackPawn]) |
           (Knight[square] & (pieces[WhiteKnight] | pieces[BlackKnight])) |
           (bishop(square, occupied) &
            (pieces[WhiteBishop] | pieces[BlackBishop] |
             pieces[WhiteQueen] | pieces[BlackQueen])) |
           (rook(square, occupied) &
            (pieces[WhiteRook] | pieces[BlackRook] |
             pieces[WhiteQueen] | pieces[BlackQueen])) |
           (King[square] & (pieces[WhiteKing] | pieces[BlackKing]));
}

}  // namespace proton::attacks
