#include "attacks.h"

#if defined(PROTON_PEXT_ATTACKS)
#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace proton::attacks {
namespace {

bool supports_pext() {
#if defined(_MSC_VER)
    int registers[4]{};
    __cpuid(registers, 0);
    if (registers[0] < 7) return false;
    __cpuidex(registers, 7, 0);
    return (registers[1] & (1 << 8)) != 0;
#else
    return __builtin_cpu_supports("bmi2");
#endif
}

Bitboard relevant_mask(int square, const std::array<int, 4>& directions) {
    Bitboard mask = 0;
    for (const int delta : directions) {
        int current = square;
        while (valid_step(current, current + delta, delta)) {
            current += delta;
            if (valid_step(current, current + delta, delta)) mask |= bit(current);
        }
    }
    return mask;
}

template <std::size_t Size>
void build_table(const std::array<int, 4>& directions,
                 std::array<Bitboard, 64>& masks,
                 std::array<std::uint32_t, 64>& offsets,
                 std::array<Bitboard, Size>& moves,
                 Bitboard (*reference)(int, Bitboard)) {
    std::uint32_t offset = 0;
    for (int square = 0; square < 64; ++square) {
        const Bitboard mask = relevant_mask(square, directions);
        masks[square] = mask;
        offsets[square] = offset;
        Bitboard subset = 0;
        do {
            moves[offset++] = reference(square, subset);
            subset = (subset - mask) & mask;
        } while (subset != 0);
    }
}

}

const bool HardwarePext = supports_pext();

SlidingTables::SlidingTables() {
    if (!HardwarePext) return;
    build_table(BishopDirections, bishop_masks, bishop_offsets, bishop_moves, bishop_rays);
    build_table(RookDirections, rook_masks, rook_offsets, rook_moves, rook_rays);
}

const SlidingTables Sliding{};

}
#endif
