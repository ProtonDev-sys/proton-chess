#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include "eval.h"

namespace proton {

struct SearchLimits {
    int depth = 64;
    int movetime_ms = 0;
    bool infinite = false;
    int wtime_ms = 0;
    int btime_ms = 0;
    int winc_ms = 0;
    int binc_ms = 0;
};

struct SearchResult {
    Move best_move = Move::null();
    int score_cp = 0;
    int depth = 0;
    std::uint64_t nodes = 0;
};

class Search {
public:
    Search();

    void set_options(const EngineOptions& options);
    void new_game();
    SearchResult find_best_move(Position& position, const SearchLimits& limits, Evaluator& evaluator);

private:
    enum Bound : std::uint8_t {
        BoundNone = 0,
        BoundUpper = 1,
        BoundLower = 2,
        BoundExact = 3
    };

    struct TTEntry {
        std::uint64_t key = 0;
        int depth = -1;
        int score = 0;
        Move best = Move::null();
        Bound bound = BoundNone;
    };

    static constexpr int MaxPly = 64;
    static constexpr int Infinity = 32000;
    static constexpr int MateScore = 30000;

    EngineOptions options_{};
    std::vector<TTEntry> table_{};
    std::uint64_t nodes_ = 0;
    std::chrono::steady_clock::time_point deadline_{};
    bool stop_ = false;
    std::array<std::array<Move, 2>, MaxPly> killers_{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};
    Evaluator* evaluator_ = nullptr;

    void resize_table(int hash_mb);
    void clear_heuristics();
    [[nodiscard]] bool time_expired() const;
    [[nodiscard]] int score_move(const Position& position, const Move& move, const Move& tt_move, int ply) const;
    [[nodiscard]] int quiescence(Position& position, int alpha, int beta, int ply);
    [[nodiscard]] int negamax(Position& position, int depth, int alpha, int beta, int ply, bool allow_null);
    void store_tt(std::uint64_t key, int depth, int score, Bound bound, const Move& best);
    [[nodiscard]] const TTEntry* probe_tt(std::uint64_t key) const;
};

}  // namespace proton
