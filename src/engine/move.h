#pragma once

#include <cstdint>
#include <string>

#include "types.h"

namespace proton {

enum MoveFlag : std::uint8_t {
    MoveQuiet = 0,
    MoveCapture = 1 << 0,
    MoveDoublePush = 1 << 1,
    MoveKingCastle = 1 << 2,
    MoveQueenCastle = 1 << 3,
    MoveEnPassant = 1 << 4,
    MovePromotion = 1 << 5
};

struct Move {
    std::uint8_t from = 64;
    std::uint8_t to = 64;
    PieceType promotion = NoPieceType;
    std::uint8_t flags = MoveQuiet;

    [[nodiscard]] constexpr bool is_null() const { return from >= 64 || to >= 64; }
    [[nodiscard]] constexpr bool is_capture() const { return (flags & MoveCapture) != 0; }
    [[nodiscard]] constexpr bool is_promotion() const { return (flags & MovePromotion) != 0; }
    [[nodiscard]] constexpr bool is_castle() const {
        return (flags & (MoveKingCastle | MoveQueenCastle)) != 0;
    }

    [[nodiscard]] std::string to_uci() const {
        if (is_null()) return "0000";
        std::string out = square_to_string(from) + square_to_string(to);
        if (is_promotion()) {
            char suffix = 'q';
            switch (promotion) {
            case Knight: suffix = 'n'; break;
            case Bishop: suffix = 'b'; break;
            case Rook: suffix = 'r'; break;
            case Queen: suffix = 'q'; break;
            default: break;
            }
            out.push_back(suffix);
        }
        return out;
    }

    [[nodiscard]] static constexpr Move null() { return Move{}; }
};

constexpr bool operator==(const Move& lhs, const Move& rhs) {
    return lhs.from == rhs.from && lhs.to == rhs.to &&
           lhs.promotion == rhs.promotion && lhs.flags == rhs.flags;
}

constexpr bool operator!=(const Move& lhs, const Move& rhs) { return !(lhs == rhs); }

}  // namespace proton
