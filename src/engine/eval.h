#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "position.h"

namespace proton {

inline constexpr int UciEloMin = 800;
inline constexpr int UciEloDefault = 2800;
inline constexpr int UciEloLegacyTop = 2800;
inline constexpr int UciEloMax = 3000;

struct UciEloProfile {
    int skill = 20;
    int max_loss_cp = 0;
    int variety_percent = 0;
};

[[nodiscard]] constexpr UciEloProfile uci_elo_profile(int elo) {
    const int bounded = elo < UciEloMin ? UciEloMin :
                        (elo > UciEloMax ? UciEloMax : elo);
    if (bounded <= UciEloLegacyTop) {
        const int loss = (UciEloLegacyTop - bounded) / 8;
        constexpr int EloSpan = UciEloLegacyTop - UciEloMin;
        const int variety = 15 +
            ((UciEloLegacyTop - bounded) * 70 + EloSpan / 2) / EloSpan;
        return UciEloProfile{
            (bounded - UciEloMin) / 100,
            loss < 8 ? 8 : (loss > 250 ? 250 : loss),
            variety,
        };
    }
    const int high_elo_span = UciEloMax - UciEloLegacyTop;
    const int scaled_loss = (UciEloMax - bounded) * 8;
    const int variety = 5 +
        ((UciEloMax - bounded) * 10 + high_elo_span / 2) / high_elo_span;
    return UciEloProfile{
        20,
        (scaled_loss + high_elo_span - 1) / high_elo_span,
        variety,
    };
}

struct EngineOptions {
    int hash_mb = 64;

    bool use_book = true;
    std::string book_file = "openings/book_lines.txt";
    int book_randomness = 0;       // 0 = best-weight line, 100 = full weighted variety.
    bool uci_limit_strength = false;
    int uci_elo = UciEloDefault;
    bool human_style = false;
    int human_skill = 20;          // 0..20. 20 only varies between near-equal moves.
    int human_max_loss_cp = 12;    // Maximum intentional loss at skill 20.
    int human_variety_percent = 35;  // Chance to consider a verified alternative.
    int move_overhead_ms = 25;
    int contempt_cp = 0;
    std::uint64_t human_seed = 0;
};

class StaticEvaluator {
public:
    StaticEvaluator();
    [[nodiscard]] int evaluate(const Position& position) const;
    void clear_cache();

private:
    struct PawnCacheEntry {
        std::uint64_t key = 0;
        int mg = 0;
        int eg = 0;
        std::array<std::array<std::uint8_t, 8>, 2> files{};
        std::array<Bitboard, 2> attacks{};
        std::array<Bitboard, 2> passed{};
        bool valid = false;
    };

    [[nodiscard]] const PawnCacheEntry& pawn_info(const Position& position) const;
    mutable std::vector<PawnCacheEntry> pawn_cache_{};
};

class Evaluator {
public:
    Evaluator();
    void set_options(const EngineOptions& options);
    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] std::vector<float> policy_scores(const Position& position,
                                                    const std::vector<Move>& moves) const;
    void clear_cache();

private:
    struct CacheEntry {
        std::uint64_t key = 0;
        int score = 0;
        bool valid = false;
    };

    StaticEvaluator core_{};
    mutable std::vector<CacheEntry> cache_{};
};

}  // namespace proton
