// Build: c++ -std=c++20 -O3 -Isrc tools/benchmark_attacks.cpp -o benchmark-attacks
// This is a slider microbenchmark, not a whole-engine NPS or Elo measurement.
#include "engine/attacks.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

namespace {
using proton::Bitboard;
struct Input { Bitboard occupied; int square; };
volatile Bitboard sink = 0;

// Exact pre-optimization algorithm from main (a04cac80ce4f5514...).
inline Bitboard baseline_ray(int direction, int square, Bitboard occupied) {
    Bitboard result = proton::attacks::Rays[direction][square];
    const Bitboard blockers = result & occupied;
    if (blockers == 0) return result;
    const int blocker = proton::attacks::RayDirections[direction] > 0
        ? static_cast<int>(std::countr_zero(blockers))
        : 63 - static_cast<int>(std::countl_zero(blockers));
    return result ^ proton::attacks::Rays[direction][blocker];
}
inline Bitboard baseline_queen(int square, Bitboard occupied) {
    return baseline_ray(0, square, occupied) | baseline_ray(1, square, occupied) |
           baseline_ray(2, square, occupied) | baseline_ray(3, square, occupied) |
           baseline_ray(4, square, occupied) | baseline_ray(5, square, occupied) |
           baseline_ray(6, square, occupied) | baseline_ray(7, square, occupied);
}
std::uint64_t random_word(std::uint64_t& state) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}
template<bool Candidate>
std::pair<double, Bitboard> measure(const std::vector<Input>& inputs) {
    const auto start = std::chrono::steady_clock::now();
    Bitboard checksum = 0;
    for (int repeat = 0; repeat < 8; ++repeat) {
        for (const auto& input : inputs) {
            const Bitboard result = Candidate
                ? proton::attacks::queen(input.square, input.occupied)
                : baseline_queen(input.square, input.occupied);
            checksum = (checksum ^ result) * 0x100000001b3ULL;
        }
    }
    sink = checksum;
    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return {ms, checksum};
}
}  // namespace

int main() {
    std::cout << "occupancy,baseline_median_ms,candidate_median_ms,speedup,checksum\n";
    for (int density : {0, 1, 2, 3, 4}) {
        std::uint64_t state = 20260907;
        std::vector<Input> inputs;
        inputs.reserve(1 << 18);
        for (int i = 0; i < (1 << 18); ++i) {
            Bitboard occupied = random_word(state);
            if (density == 0) occupied = 0;
            if (density == 1) occupied &= random_word(state) & random_word(state);
            if (density == 2) occupied &= random_word(state);
            if (density == 4) occupied = ~Bitboard{0};
            inputs.push_back({occupied, static_cast<int>(random_word(state) & 63)});
        }
        (void)measure<false>(inputs); (void)measure<true>(inputs);
        std::array<double, 9> baseline{}, candidate{};
        Bitboard checksum = 0;
        for (int trial = 0; trial < 9; ++trial) {
            std::pair<double, Bitboard> before, after;
            if (trial % 2 == 0) {
                before = measure<false>(inputs); after = measure<true>(inputs);
            } else {
                after = measure<true>(inputs); before = measure<false>(inputs);
            }
            if (before.second != after.second) {
                std::cerr << "Mismatched checksums\n"; return EXIT_FAILURE;
            }
            baseline[trial] = before.first; candidate[trial] = after.first;
            checksum = after.second;
        }
        std::sort(baseline.begin(), baseline.end());
        std::sort(candidate.begin(), candidate.end());
        constexpr std::array<const char*, 5> labels = {"empty", "12.5%", "25%", "50%", "full"};
        std::cout << labels[density] << ',' << std::fixed << std::setprecision(3)
                  << baseline[4] << ',' << candidate[4] << ','
                  << baseline[4] / candidate[4] << ',' << checksum << '\n';
    }
}
