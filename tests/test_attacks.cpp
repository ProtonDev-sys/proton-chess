#include "engine/attacks.h"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>

namespace {
using proton::Bitboard;
// Deliberately independent of valid_step(), Rays, and all production sliders.
constexpr std::array<int, 8> Files = {0, 0, 1, -1, 1, -1, 1, -1};
constexpr std::array<int, 8> Ranks = {1, -1, 0, 0, 1, 1, -1, -1};

Bitboard scan(int square, Bitboard occupied, int first, int last) {
    Bitboard result = 0;
    for (int direction = first; direction < last; ++direction) {
        int file = square % 8 + Files[direction];
        int rank = square / 8 + Ranks[direction];
        while (file >= 0 && file < 8 && rank >= 0 && rank < 8) {
            const Bitboard target = Bitboard{1} << (rank * 8 + file);
            result |= target;
            if (occupied & target) break;
            file += Files[direction];
            rank += Ranks[direction];
        }
    }
    return result;
}

void check(Bitboard actual, Bitboard expected, int square, Bitboard occupied) {
    if (actual != expected) {
        std::cerr << "attack mismatch at square " << square << " occupied="
                  << occupied << " expected=" << expected << " actual=" << actual << '\n';
        std::exit(EXIT_FAILURE);
    }
}

std::uint64_t random_word(std::uint64_t& state) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}
}  // namespace

int main() {
    std::uint64_t cases = 0;
    for (int square = 0; square < 64; ++square) {
        for (int direction = 0; direction < 8; ++direction) {
            const Bitboard mask = scan(square, 0, direction, direction + 1);
            Bitboard subset = 0;
            do {
                check(proton::attacks::ray(direction, square, subset),
                      scan(square, subset, direction, direction + 1), square, subset);
                // Off-ray occupancy, including the origin, must have no effect.
                check(proton::attacks::ray(direction, square, subset | ~mask),
                      scan(square, subset, direction, direction + 1), square, subset | ~mask);
                subset = (subset - mask) & mask;
                ++cases;
            } while (subset);
        }
        for (int first : {0, 4}) {
            const Bitboard mask = scan(square, 0, first, first + 4);
            Bitboard subset = 0;
            do {
                const auto slider = first == 0 ? proton::attacks::rook : proton::attacks::bishop;
                const Bitboard expected = scan(square, subset, first, first + 4);
                check(slider(square, subset), expected, square, subset);
                check(slider(square, subset | ~mask), expected, square, subset | ~mask);
                subset = (subset - mask) & mask;
                ++cases;
            } while (subset);
        }
    }
    std::uint64_t state = 20260907;
    for (int i = 0; i < 100000; ++i) {
        const Bitboard occupied = random_word(state);
        const int square = static_cast<int>(random_word(state) & 63);
        check(proton::attacks::queen(square, occupied), scan(square, occupied, 0, 8), square, occupied);
        ++cases;
    }
    std::cout << "Passed " << cases << " exhaustive/random attack cases (including off-ray occupancy)\n";
}
