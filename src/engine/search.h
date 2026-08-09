#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "eval.h"

namespace proton {

struct SearchLimits {
    int depth = 0;
    int movetime_ms = 0;
    int white_time_ms = 0;
    int black_time_ms = 0;
    int white_increment_ms = 0;
    int black_increment_ms = 0;
    int moves_to_go = 0;
    std::uint64_t node_limit = 0;
    bool infinite = false;
    bool ponder = false;
    bool search_moves_specified = false;
    std::vector<Move> search_moves;
    const std::atomic<bool>* external_stop = nullptr;
    const std::chrono::steady_clock::time_point* external_deadline = nullptr;
};

struct SearchInfo {
    int depth = 0;
    int selective_depth = 0;
    int score_cp = 0;
    std::uint64_t nodes = 0;
    std::uint64_t nps = 0;
    int time_ms = 0;
    int hashfull_permille = 0;
    std::vector<Move> pv;
};

struct SearchResult {
    Move best = Move::null();
    Move ponder = Move::null();
    int score_cp = 0;
    int depth = 0;
    int selective_depth = 0;
    std::uint64_t nodes = 0;
    int time_ms = 0;
    std::vector<Move> pv;
};

class Search {
public:
    using InfoCallback = std::function<void(const SearchInfo&)>;
    using StartCallback = std::function<void()>;

    explicit Search(Evaluator& evaluator,
                    const EngineOptions& initial_options = EngineOptions{});

    void set_options(const EngineOptions& options);
    void new_game();
    void request_stop();
    void begin_ponder();
    void ponder_hit();
    [[nodiscard]] bool is_searching() const { return searching_.load(std::memory_order_relaxed); }

    SearchResult think(Position position, const SearchLimits& limits,
                       const InfoCallback& callback = {},
                       const StartCallback& start_callback = {});

    [[nodiscard]] static constexpr int mate_score() { return 32000; }
    [[nodiscard]] static constexpr int mate_threshold() { return 31800; }

private:
    static constexpr int MaxPly = 128;
    static constexpr int Infinity = 32767;
    static constexpr int MateScore = 32000;
    static constexpr int MateThreshold = 31800;
    static constexpr int NoScore = -40000;
    static constexpr int HistoryStateCount = 13 * 64;
    static constexpr int CorrectionSize = 1 << 14;
    static constexpr int CorrectionScale = 128;
    static constexpr std::int16_t TTNoEval = 32767;

    enum class Bound : std::uint8_t { None, Upper, Lower, Exact };

    struct TTEntry {
        std::uint32_t key = 0;
        Move move = Move::null();
        std::int16_t score = 0;
        std::int16_t static_eval = TTNoEval;
        std::int8_t depth = -128;
        Bound bound = Bound::None;
        std::uint8_t generation = 0;
        std::uint8_t padding = 0;
    };

    struct alignas(64) TTBucket {
        std::array<TTEntry, 4> entries{};
    };

    static_assert(sizeof(TTEntry) == 16);
    static_assert(sizeof(TTBucket) == 64);

    struct ScoredMove {
        Move move = Move::null();
        int score = 0;
        int see = 0;
    };

    struct RootMove {
        Move move = Move::null();
        int score = NoScore;
        int previous_score = NoScore;
        float policy = 0.0F;
        bool exact = false;
        std::vector<Move> pv;
    };

    struct BookMove {
        Move move = Move::null();
        int weight = 1;
    };

    Evaluator& evaluator_;
    EngineOptions options_{};
    SearchLimits limits_{};

    std::vector<TTBucket> table_{};
    std::size_t table_mask_ = 0;
    std::uint8_t generation_ = 0;

    enum class PonderState : std::uint8_t { Inactive, Pondering, Hit };

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> searching_{false};
    std::atomic<PonderState> ponder_state_{PonderState::Inactive};
    std::uint64_t nodes_ = 0;
    int selective_depth_ = 0;
    std::chrono::steady_clock::time_point start_time_{};
    std::chrono::steady_clock::time_point soft_deadline_{};
    std::chrono::steady_clock::time_point hard_deadline_{};
    bool has_soft_deadline_ = false;
    bool has_hard_deadline_ = false;
    bool ponder_time_activated_ = false;
    int soft_time_budget_ms_ = 0;
    int hard_time_budget_ms_ = 0;

    std::array<std::array<Move, 2>, MaxPly> killers_{};
    std::array<std::array<std::array<int, 64>, 64>, 2> history_{};
    std::array<std::array<std::array<int, 6>, 64>, 13> capture_history_{};
    std::array<std::array<Move, 64>, 13> counter_moves_{};
    std::vector<std::int16_t> continuation_history_{};
    std::vector<std::int16_t> continuation_history_2_{};
    std::array<std::array<int, CorrectionSize>, 2> correction_history_{};
    std::array<int, MaxPly> eval_stack_{};
    std::array<Move, MaxPly> move_stack_{};
    std::array<Piece, MaxPly> moved_piece_stack_{};
    std::array<std::vector<Move>, MaxPly> generated_moves_{};
    std::array<std::vector<Move>, MaxPly> tried_quiets_{};
    std::array<std::vector<Move>, MaxPly> tried_captures_{};
    std::array<std::vector<ScoredMove>, MaxPly> move_lists_{};
    std::array<std::array<Move, MaxPly>, MaxPly> pv_table_{};
    std::array<int, MaxPly> pv_length_{};

    std::unordered_map<std::uint64_t, std::vector<BookMove>> book_{};
    std::mt19937_64 random_{};
    Color root_side_ = White;
    bool verification_search_ = false;

    void resize_hash(int megabytes);
    void clear_hash();
    [[nodiscard]] TTEntry* probe(std::uint64_t key);
    [[nodiscard]] const TTEntry* probe(std::uint64_t key) const;
    void store(std::uint64_t key, int depth, int score, int static_eval,
               Bound bound, const Move& move, int ply);
    [[nodiscard]] int hashfull() const;
    [[nodiscard]] static int score_to_tt(int score, int ply);
    [[nodiscard]] static int score_from_tt(int score, int ply);
    [[nodiscard]] static std::uint32_t tt_signature(std::uint64_t key);

    void configure_time(const Position& position);
    void activate_time_budget(std::chrono::steady_clock::time_point now);
    void activate_ponder_time_if_needed();
    [[nodiscard]] bool should_stop(bool force_time_check = false);
    [[nodiscard]] bool soft_time_expired() const;
    [[nodiscard]] int elapsed_ms() const;

    int alpha_beta(Position& position, int depth, int alpha, int beta, int ply,
                   bool pv_node, bool cut_node, bool allow_null,
                   const Move& previous_move);
    int quiescence(Position& position, int alpha, int beta, int ply);
    [[nodiscard]] Move find_quiet_mate(Position& position, int ply);
    int search_root(Position& position, std::vector<RootMove>& root_moves,
                    int depth, int alpha, int beta);

    void score_moves(const Position& position, const std::vector<Move>& moves,
                              int ply, const Move& tt_move, bool captures_only);
    [[nodiscard]] int move_order_score(const Position& position, const Move& move,
                                       int ply, const Move& tt_move,
                                       bool captures_only, int see) const;
    [[nodiscard]] static int captured_value(const Position& position, const Move& move);
    [[nodiscard]] static int promotion_gain(const Move& move);
    void update_quiet_history(const Position& position, Color color, const Move& best,
                              int depth, const std::vector<Move>& tried_quiets, int ply);
    void update_capture_history(const Position& position, const Move& best, int depth,
                                const std::vector<Move>& tried_captures);
    static void apply_history_bonus(int& value, int bonus);
    static void apply_continuation_bonus(std::int16_t& value, int bonus);
    [[nodiscard]] int continuation_score(const Position& position,
                                         const Move& move, int ply) const;
    void update_correction_history(const Position& position, int depth,
                                   int raw_eval, int score, Bound bound);
    [[nodiscard]] int corrected_static_eval(const Position& position,
                                            int raw_eval) const;

    void update_pv(int ply, const Move& move);
    [[nodiscard]] std::vector<Move> current_pv(int ply = 0) const;

    void build_builtin_book();
    bool load_book_file(const std::string& path);
    void add_book_line(const std::vector<std::string>& moves, int weight);
    [[nodiscard]] Move select_book_move(const Position& position);
    [[nodiscard]] Move select_human_move(const Position& position,
                                         std::vector<RootMove>& root_moves,
                                         int completed_depth);
    [[nodiscard]] std::optional<SearchResult> confirm_human_candidate(
        const Position& position, const Move& candidate, int completed_depth);

    [[nodiscard]] int draw_score(const Position& position) const;
    [[nodiscard]] int rule50_score(Position& position, int ply, bool in_check);

    [[nodiscard]] static int lmr_reduction(int depth, int move_number,
                                           bool pv_node, bool improving);
};

}  // namespace proton
