#include "search.h"

#include "see.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <limits>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <utility>

namespace proton {
namespace {

constexpr int RootPolicyScale = 24;
constexpr int QuietMateScanMaxPly = 3;

struct HumanSettings {
    bool enabled = false;
    int skill = 20;
    int max_loss_cp = 12;
    int variety_percent = 0;
};

HumanSettings resolved_human_settings(const EngineOptions& options) {
    if (options.uci_limit_strength) {
        const UciEloProfile profile = uci_elo_profile(options.uci_elo);
        return HumanSettings{
            true,
            profile.skill,
            profile.max_loss_cp,
            profile.variety_percent,
        };
    }
    return HumanSettings{
        options.human_style || options.human_skill < 20,
        std::clamp(options.human_skill, 0, 20),
        std::clamp(options.human_max_loss_cp, 0, 500),
        std::clamp(options.human_variety_percent, 0, 100),
    };
}

bool human_selection_opportunity(const HumanSettings& human,
                                 std::mt19937_64& random) {
    if (!human.enabled || human.variety_percent <= 0) return false;
    if (human.variety_percent >= 100) return true;

    // The engine's raw generator is specified; distribution implementations are
    // not. A direct draw keeps seeded opportunity decisions reproducible across
    // standard libraries. The modulo bias is negligible for a 64-bit source.
    return random() % 100ULL <
           static_cast<std::uint64_t>(human.variety_percent);
}

int human_loss_allowance(const HumanSettings& human) {
    const int skill_gap = 20 - human.skill;
    return std::clamp(human.max_loss_cp + skill_gap * 8 +
                      skill_gap * skill_gap * 2, 0, 700);
}

bool same_move(const Move& lhs, const Move& rhs) {
    return !lhs.is_null() && !rhs.is_null() && lhs == rhs;
}

bool is_quiet(const Move& move) {
    return !move.is_capture() && !move.is_promotion();
}

int clamp_score(int score) {
    return std::clamp(score, -Search::mate_score(), Search::mate_score());
}

}  // namespace

Search::Search(Evaluator& evaluator, const EngineOptions& initial_options)
    : evaluator_(evaluator),
      options_(initial_options),
      continuation_history_(HistoryStateCount * HistoryStateCount, 0),
      continuation_history_2_(HistoryStateCount * HistoryStateCount, 0) {
    options_.hash_mb = std::clamp(options_.hash_mb, 1, 4096);
    for (int ply = 0; ply < MaxPly; ++ply) {
        generated_moves_[ply].reserve(96);
        tried_quiets_[ply].reserve(64);
        tried_captures_[ply].reserve(48);
        move_lists_[ply].reserve(96);
    }
    resize_hash(options_.hash_mb);
    if (options_.use_book) build_builtin_book();
    if (options_.human_seed != 0) {
        random_.seed(options_.human_seed);
    } else {
        std::random_device device;
        random_.seed((static_cast<std::uint64_t>(device()) << 32U) ^ device());
    }
}

void Search::set_options(const EngineOptions& options) {
    const bool hash_changed = options.hash_mb != options_.hash_mb;
    const bool book_changed = options.book_file != options_.book_file;
    const bool build_book = options.use_book && (!options_.use_book || book_changed);
    const bool seed_changed = options.human_seed != options_.human_seed;
    const bool contempt_changed = options.contempt_cp != options_.contempt_cp;
    options_ = options;
    if (hash_changed) resize_hash(options_.hash_mb);
    else if (contempt_changed) clear_hash();
    if (build_book) build_builtin_book();
    if (seed_changed) {
        if (options_.human_seed != 0) {
            random_.seed(options_.human_seed);
        } else {
            std::random_device device;
            random_.seed((static_cast<std::uint64_t>(device()) << 32U) ^ device());
        }
    }
}

void Search::new_game() {
    request_stop();
    clear_hash();
    for (auto& side : history_) {
        for (auto& from : side) from.fill(0);
    }
    for (auto& row : counter_moves_) row.fill(Move::null());
    for (auto& pair : killers_) pair.fill(Move::null());
    for (auto& piece : capture_history_) {
        for (auto& square : piece) square.fill(0);
    }
    std::fill(continuation_history_.begin(), continuation_history_.end(), 0);
    std::fill(continuation_history_2_.begin(), continuation_history_2_.end(), 0);
    for (auto& side : correction_history_) side.fill(0);
    evaluator_.clear_cache();
}

void Search::request_stop() { stop_requested_.store(true, std::memory_order_relaxed); }

void Search::begin_ponder() {
    ponder_state_.store(PonderState::Pondering, std::memory_order_release);
}

void Search::ponder_hit() {
    PonderState expected = PonderState::Pondering;
    ponder_state_.compare_exchange_strong(expected, PonderState::Hit,
                                          std::memory_order_acq_rel,
                                          std::memory_order_relaxed);
}

void Search::resize_hash(int megabytes) {
    const std::size_t bytes = static_cast<std::size_t>(std::clamp(megabytes, 1, 4096)) *
                              1024ULL * 1024ULL;
    std::size_t buckets = std::max<std::size_t>(1, bytes / sizeof(TTBucket));
    std::size_t power_of_two = 1;
    while ((power_of_two << 1U) <= buckets) power_of_two <<= 1U;
    table_.assign(power_of_two, TTBucket{});
    table_mask_ = power_of_two - 1;
}

void Search::clear_hash() {
    for (TTBucket& bucket : table_) bucket = TTBucket{};
    generation_ = 0;
}

Search::TTEntry* Search::probe(std::uint64_t key) {
    if (table_.empty()) return nullptr;
    TTBucket& bucket = table_[static_cast<std::size_t>(key) & table_mask_];
    const std::uint32_t signature = tt_signature(key);
    for (TTEntry& entry : bucket.entries) {
        if (entry.bound != Bound::None && entry.key == signature) return &entry;
    }
    return nullptr;
}

const Search::TTEntry* Search::probe(std::uint64_t key) const {
    if (table_.empty()) return nullptr;
    const TTBucket& bucket = table_[static_cast<std::size_t>(key) & table_mask_];
    const std::uint32_t signature = tt_signature(key);
    for (const TTEntry& entry : bucket.entries) {
        if (entry.bound != Bound::None && entry.key == signature) return &entry;
    }
    return nullptr;
}

void Search::store(std::uint64_t key, int depth, int score, int static_eval,
                   Bound bound, const Move& move, int ply) {
    if (table_.empty() || search_aborted()) return;

    TTBucket& bucket = table_[static_cast<std::size_t>(key) & table_mask_];
    const std::uint32_t signature = tt_signature(key);
    TTEntry* replacement = &bucket.entries.front();
    int replacement_value = std::numeric_limits<int>::max();

    for (TTEntry& entry : bucket.entries) {
        if (entry.bound == Bound::None || entry.key == signature) {
            replacement = &entry;
            break;
        }
        const int age = static_cast<std::uint8_t>(generation_ - entry.generation);
        const int value = static_cast<int>(entry.depth) - age * 8 +
                          (entry.bound == Bound::Exact ? 4 : 0);
        if (value < replacement_value) {
            replacement_value = value;
            replacement = &entry;
        }
    }

    const bool same_key = replacement->key == signature &&
                          replacement->bound != Bound::None;
    if (same_key) {
        // A current move is still useful for ordering even when the new search
        // is too shallow to replace the stored value. Keep the two decisions
        // independent so a shallow bound cannot discard a deeper result.
        if (!move.is_null()) replacement->move = move;

        if (depth + 2 < replacement->depth && bound != Bound::Exact) {
            replacement->generation = generation_;
            return;
        }
    }

    replacement->key = signature;
    replacement->score = static_cast<std::int16_t>(score_to_tt(clamp_score(score), ply));
    replacement->static_eval = static_eval == NoScore
        ? TTNoEval
        : static_cast<std::int16_t>(std::clamp(static_eval, -32767, 32766));
    replacement->depth = static_cast<std::int8_t>(std::clamp(depth, -1, MaxPly - 2));
    replacement->bound = bound;
    replacement->generation = generation_;
    if (!same_key || !move.is_null() || replacement->move.is_null()) {
        replacement->move = move;
    }
}

int Search::hashfull() const {
    if (table_.empty()) return 0;
    const std::size_t sample = std::min<std::size_t>(250, table_.size());
    int used = 0;
    for (std::size_t i = 0; i < sample; ++i) {
        for (const TTEntry& entry : table_[i].entries) {
            if (entry.bound != Bound::None && entry.generation == generation_) ++used;
        }
    }
    return static_cast<int>((used * 1000ULL) / (sample * 4ULL));
}

int Search::score_to_tt(int score, int ply) {
    if (score >= MateThreshold) return score + ply;
    if (score <= -MateThreshold) return score - ply;
    return score;
}

int Search::score_from_tt(int score, int ply) {
    if (score >= MateThreshold) return score - ply;
    if (score <= -MateThreshold) return score + ply;
    return score;
}

std::uint32_t Search::tt_signature(std::uint64_t key) {
    return static_cast<std::uint32_t>(key) ^ static_cast<std::uint32_t>(key >> 32U);
}

int Search::elapsed_ms() const {
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time_).count());
}

void Search::configure_time(const Position& position) {
    has_soft_deadline_ = false;
    has_hard_deadline_ = false;
    has_main_deadline_ = false;
    soft_time_budget_ms_ = 0;
    hard_time_budget_ms_ = 0;
    ponder_time_activated_ = !limits_.ponder;

    if (limits_.infinite) return;

    const int overhead = std::max(0, options_.move_overhead_ms);
    if (limits_.movetime_ms > 0) {
        const int available = std::max(1, limits_.movetime_ms - overhead);
        soft_time_budget_ms_ = std::max(1, available * 88 / 100);
        hard_time_budget_ms_ = available;
    } else {
        const bool white = position.side_to_move() == White;
        const int clock = white ? limits_.white_time_ms : limits_.black_time_ms;
        const int increment = white ? limits_.white_increment_ms : limits_.black_increment_ms;
        if (clock <= 0) return;

        const int remaining = std::max(1, clock - overhead);
        int moves_to_go = limits_.moves_to_go;
        if (moves_to_go <= 0) {
            // Spend more in the opening/middlegame, while retaining a safety reserve.
            moves_to_go = std::clamp(38 - position.fullmove_number() / 2, 14, 32);
        }

        const int base = remaining / std::max(1, moves_to_go);
        int optimum = base + increment * 3 / 4;
        optimum = std::clamp(optimum, 1, std::max(1, remaining / 4));

        int maximum = std::max(optimum + 20, optimum * 3);
        maximum = std::min(maximum, std::max(1, remaining * 2 / 3));
        maximum = std::max(maximum, optimum);
        soft_time_budget_ms_ = optimum;
        hard_time_budget_ms_ = maximum;
    }

    if (!limits_.ponder) activate_time_budget(start_time_);
}

void Search::activate_time_budget(std::chrono::steady_clock::time_point now) {
    if (soft_time_budget_ms_ > 0) {
        soft_deadline_ = now + std::chrono::milliseconds(soft_time_budget_ms_);
        has_soft_deadline_ = true;
    }
    if (hard_time_budget_ms_ > 0) {
        hard_deadline_ = now + std::chrono::milliseconds(hard_time_budget_ms_);
        has_hard_deadline_ = true;
        if (main_phase_) {
            const int main_budget_ms = std::max(1, hard_time_budget_ms_ / 2);
            main_deadline_ = now + std::chrono::milliseconds(main_budget_ms);
            if (has_soft_deadline_ && soft_deadline_ < main_deadline_) {
                main_deadline_ = soft_deadline_;
            }
            has_main_deadline_ = true;
        }
    }
}

void Search::activate_ponder_time_if_needed() {
    if (!limits_.ponder || ponder_time_activated_ ||
        ponder_state_.load(std::memory_order_acquire) != PonderState::Hit) {
        return;
    }
    ponder_time_activated_ = true;
    activate_time_budget(std::chrono::steady_clock::now());
}

bool Search::should_stop(bool force_time_check) {
    activate_ponder_time_if_needed();
    for (const std::atomic<bool>* external_stop : limits_.external_stops) {
        if (external_stop != nullptr &&
            external_stop->load(std::memory_order_relaxed)) {
            stop_requested_.store(true, std::memory_order_relaxed);
            return true;
        }
    }
    if (limits_.external_deadline != nullptr &&
        std::chrono::steady_clock::now() >= *limits_.external_deadline) {
        stop_requested_.store(true, std::memory_order_relaxed);
        return true;
    }
    if (stop_requested_.load(std::memory_order_relaxed)) return true;
    if (limits_.node_limit != 0 && nodes_ >= limits_.node_limit) {
        stop_requested_.store(true, std::memory_order_relaxed);
        return true;
    }
    if (main_phase_ && main_node_limit_ != 0 && nodes_ >= main_node_limit_) {
        main_budget_exhausted_ = true;
        return true;
    }
    if (!force_time_check && (nodes_ & 1023ULL) != 0) return false;
    if (has_hard_deadline_ && std::chrono::steady_clock::now() >= hard_deadline_) {
        stop_requested_.store(true, std::memory_order_relaxed);
        return true;
    }
    if (main_phase_ && has_main_deadline_ &&
        std::chrono::steady_clock::now() >= main_deadline_) {
        main_budget_exhausted_ = true;
        return true;
    }
    return false;
}

bool Search::search_aborted() const {
    return stop_requested_.load(std::memory_order_relaxed) || main_budget_exhausted_;
}

bool Search::soft_time_expired() const {
    return has_soft_deadline_ && std::chrono::steady_clock::now() >= soft_deadline_;
}

int Search::captured_value(const Position& position, const Move& move) {
    if (!move.is_capture()) return 0;
    if ((move.flags & MoveEnPassant) != 0) return piece_value(Pawn);
    return piece_value(piece_type(position.piece_at(move.to)));
}

int Search::promotion_gain(const Move& move) {
    return move.is_promotion() ? piece_value(move.promotion) - piece_value(Pawn) : 0;
}

int Search::move_order_score(
    const Position& position, const Move& move, int ply,
    const Move& tt_move, bool captures_only, int see,
    const ContinuationRows& continuation_rows) const {
    if (same_move(move, tt_move)) return 30'000'000;

    const Piece attacker_piece = position.piece_at(move.from);
    const PieceType attacker = piece_type(attacker_piece);
    if (move.is_capture() || move.is_promotion()) {
        int score = see >= 0 ? 20'000'000 : -2'000'000;
        const Piece victim_piece = (move.flags & MoveEnPassant) != 0
            ? make_piece(opposite(position.side_to_move()), Pawn)
            : position.piece_at(move.to);
        const PieceType victim_type = piece_type(victim_piece);
        score += captured_value(position, move) * 32;
        score -= piece_value(attacker);
        score += promotion_gain(move) * 24;
        score += std::clamp(see, -2000, 2000) * 64;
        if ((move.flags & MoveEnPassant) != 0) score += 64;
        if (move.is_capture() && attacker_piece != Empty && victim_type != NoPieceType) {
            score += capture_history_[attacker_piece][move.to][victim_type];
        }
        return score;
    }

    if (captures_only) return -1;
    if (ply < MaxPly) {
        if (same_move(move, killers_[ply][0])) return 9'000'000;
        if (same_move(move, killers_[ply][1])) return 8'000'000;
    }

    if (ply > 0 && !move_stack_[ply - 1].is_null()) {
        const Move previous = move_stack_[ply - 1];
        const Piece previous_piece = position.piece_at(previous.to);
        if (previous_piece != Empty && same_move(move, counter_moves_[previous_piece][previous.to])) {
            return 7'000'000;
        }
    }

    const Color side = position.side_to_move();
    return history_[side][move.from][move.to] +
           continuation_score(position, move, continuation_rows);
}

void Search::score_moves(const Position& position, const std::vector<Move>& moves,
                         int ply, const Move& tt_move, bool captures_only,
                         const ContinuationRows& continuation_rows) {
    std::vector<ScoredMove>& list = move_lists_[ply];
    list.clear();
    list.reserve(std::max(list.capacity(), moves.size()));
    for (const Move& move : moves) {
        const int see = (move.is_capture() || move.is_promotion())
            ? static_exchange_eval(position, move)
            : 0;
        list.push_back(ScoredMove{
            move, move_order_score(position, move, ply, tt_move, captures_only,
                                   see, continuation_rows),
            see});
    }
}

void Search::update_capture_history(const Position& position, const Move& best, int depth,
                                    const std::vector<Move>& tried_captures) {
    if (!best.is_capture()) return;

    const auto update = [&](const Move& move, int bonus) {
        const Piece attacker = position.piece_at(move.from);
        const Piece victim = (move.flags & MoveEnPassant) != 0
            ? make_piece(opposite(position.side_to_move()), Pawn)
            : position.piece_at(move.to);
        const PieceType victim_type = piece_type(victim);
        if (attacker == Empty || victim_type == NoPieceType) return;
        apply_history_bonus(capture_history_[attacker][move.to][victim_type], bonus);
    };

    const int bonus = std::min(1800, 20 * depth * depth + 24 * depth);
    update(best, bonus);
    for (const Move& move : tried_captures) {
        if (move != best) update(move, -bonus / 2);
    }
}

void Search::apply_history_bonus(int& value, int bonus) {
    constexpr int MaxHistory = 16384;
    bonus = std::clamp(bonus, -MaxHistory, MaxHistory);
    value += bonus - value * std::abs(bonus) / MaxHistory;
    value = std::clamp(value, -MaxHistory, MaxHistory);
}

void Search::apply_continuation_bonus(std::int16_t& value, int bonus) {
    constexpr int MaxHistory = 16384;
    bonus = std::clamp(bonus, -MaxHistory, MaxHistory);
    int updated = static_cast<int>(value);
    updated += bonus - updated * std::abs(bonus) / MaxHistory;
    value = static_cast<std::int16_t>(std::clamp(updated, -MaxHistory, MaxHistory));
}

Search::ContinuationRows Search::continuation_rows(int ply) const {
    ContinuationRows rows{};
    const auto row_for = [&](int offset,
                             const std::vector<std::int16_t>& table) {
        if (ply < offset) return static_cast<const std::int16_t*>(nullptr);
        const Move previous = move_stack_[ply - offset];
        const Piece previous_piece = moved_piece_stack_[ply - offset];
        if (previous.is_null() || previous_piece == Empty) {
            return static_cast<const std::int16_t*>(nullptr);
        }
        const int previous_state =
            static_cast<int>(previous_piece) * 64 + previous.to;
        return table.data() + static_cast<std::size_t>(previous_state) *
                                  HistoryStateCount;
    };
    rows[0] = row_for(1, continuation_history_);
    rows[1] = row_for(2, continuation_history_2_);
    return rows;
}

int Search::continuation_score(
    const Position& position, const Move& move,
    const ContinuationRows& continuation_rows) {
    if (move.is_null()) return 0;
    const Piece current_piece = position.piece_at(move.from);
    if (current_piece == Empty) return 0;
    const int current_state = static_cast<int>(current_piece) * 64 + move.to;
    return (continuation_rows[0] != nullptr
                ? static_cast<int>(continuation_rows[0][current_state]) * 2
                : 0) +
           (continuation_rows[1] != nullptr
                ? static_cast<int>(continuation_rows[1][current_state])
                : 0);
}

void Search::update_quiet_history(const Position& position, Color color,
                                  const Move& best, int depth,
                                  const std::vector<Move>& tried_quiets, int ply) {
    const int bonus = std::min(1800, 24 * depth * depth + 32 * depth);
    apply_history_bonus(history_[color][best.from][best.to], bonus);

    const auto update_continuation = [&](const Move& move, int update_bonus) {
        if (ply <= 0 || move.is_null()) return;
        const Piece current_piece = position.piece_at(move.from);
        if (current_piece == Empty) return;
        const int current_state = static_cast<int>(current_piece) * 64 + move.to;

        const auto update = [&](int offset, std::vector<std::int16_t>& table,
                                int scaled_bonus) {
            if (ply < offset) return;
            const Move previous = move_stack_[ply - offset];
            const Piece previous_piece = moved_piece_stack_[ply - offset];
            if (previous.is_null() || previous_piece == Empty) return;
            const int previous_state = static_cast<int>(previous_piece) * 64 + previous.to;
            const std::size_t index = static_cast<std::size_t>(previous_state) *
                                      HistoryStateCount + current_state;
            apply_continuation_bonus(table[index], scaled_bonus);
        };
        update(1, continuation_history_, update_bonus);
        update(2, continuation_history_2_, update_bonus / 2);
    };
    update_continuation(best, bonus);

    for (const Move& move : tried_quiets) {
        if (move == best) continue;
        apply_history_bonus(history_[color][move.from][move.to], -bonus / 2);
        update_continuation(move, -bonus / 2);
    }

    if (ply < MaxPly) {
        if (killers_[ply][0] != best) {
            killers_[ply][1] = killers_[ply][0];
            killers_[ply][0] = best;
        }
    }

}

int Search::corrected_static_eval(const Position& position, int raw_eval) const {
    if (raw_eval == NoScore || std::abs(raw_eval) >= MateThreshold) return raw_eval;
    const std::size_t index = static_cast<std::size_t>(position.pawn_key()) &
                              (CorrectionSize - 1);
    const int correction = correction_history_[position.side_to_move()][index] /
                           CorrectionScale;
    return std::clamp(raw_eval + correction, -MateThreshold + 1, MateThreshold - 1);
}

void Search::update_correction_history(const Position& position, int depth,
                                       int raw_eval, int score, Bound bound) {
    if (depth < 2 || raw_eval == NoScore || std::abs(raw_eval) >= MateThreshold ||
        std::abs(score) >= MateThreshold) {
        return;
    }
    if ((bound == Bound::Lower && score <= raw_eval) ||
        (bound == Bound::Upper && score >= raw_eval) || bound == Bound::None) {
        return;
    }

    const std::size_t index = static_cast<std::size_t>(position.pawn_key()) &
                              (CorrectionSize - 1);
    int& value = correction_history_[position.side_to_move()][index];
    constexpr int MaxCorrection = 16384;
    const int target = std::clamp((score - raw_eval) * CorrectionScale,
                                  -MaxCorrection, MaxCorrection);
    const int weight = std::clamp(8 + depth * 4, 12, 64);
    value += (target - value) * weight / 256;
    value = std::clamp(value, -MaxCorrection, MaxCorrection);
}

void Search::update_pv(int ply, const Move& move) {
    pv_table_[ply][ply] = move;
    const int child_length = std::clamp(pv_length_[ply + 1], ply + 1, MaxPly);
    for (int index = ply + 1; index < child_length; ++index) {
        pv_table_[ply][index] = pv_table_[ply + 1][index];
    }
    pv_length_[ply] = child_length;
}

std::vector<Move> Search::current_pv(int ply) const {
    std::vector<Move> pv;
    if (ply < 0 || ply >= MaxPly) return pv;
    const int end = std::clamp(pv_length_[ply], ply, MaxPly);
    pv.reserve(static_cast<std::size_t>(end - ply));
    for (int index = ply; index < end; ++index) {
        if (pv_table_[ply][index].is_null()) break;
        pv.push_back(pv_table_[ply][index]);
    }
    return pv;
}

int Search::draw_score(const Position& position) const {
    if (options_.contempt_cp == 0) return 0;
    return position.side_to_move() == root_side_ ? -options_.contempt_cp
                                                 : options_.contempt_cp;
}

int Search::rule50_score(Position& position, int ply, bool in_check) {
    if (position.halfmove_clock() < 100) return NoScore;
    if (!in_check) return draw_score(position);

    // Checkmate takes precedence over a rule-50 draw. This branch is rare, so
    // the small legality scan is preferable to contaminating every normal node.
    std::vector<Move>& pseudo = generated_moves_[ply];
    position.generate_pseudo_moves(pseudo);
    for (const Move& move : pseudo) {
        UndoState undo;
        if (!position.make_move(move, undo)) continue;
        position.unmake_move(move, undo);
        return draw_score(position);
    }
    return -MateScore + ply;
}

int Search::lmr_reduction(int depth, int move_number, bool pv_node, bool improving) {
    if (depth < 3 || move_number < 3) return 0;
    static const auto reductions = [] {
        std::array<std::array<std::uint8_t, MaxPly>, MaxPly> table{};
        for (int d = 3; d < MaxPly; ++d) {
            for (int move = 3; move < MaxPly; ++move) {
                table[d][move] = static_cast<std::uint8_t>(std::clamp(
                    static_cast<int>(0.75 + std::log(static_cast<double>(d)) *
                                              std::log(static_cast<double>(move)) / 2.15),
                    0, 255));
            }
        }
        return table;
    }();

    int reduction = reductions[std::min(depth, MaxPly - 1)]
                              [std::min(move_number, MaxPly - 1)];
    if (pv_node) --reduction;
    if (improving) --reduction;
    return std::clamp(reduction, 0, std::max(0, depth - 2));
}

Move Search::find_quiet_mate(Position& position, int ply) {
    std::vector<Move>& candidates = generated_moves_[ply];
    position.generate_pseudo_moves(candidates);
    std::vector<Move>& replies = generated_moves_[ply + 1];

    for (const Move& move : candidates) {
        if (should_stop()) return Move::null();
        if (!is_quiet(move)) continue;

        UndoState undo;
        if (!position.make_move(move, undo)) continue;

        bool has_legal_reply = true;
        if (position.in_check(position.side_to_move())) {
            has_legal_reply = false;
            position.generate_pseudo_moves(replies);
            for (const Move& reply : replies) {
                UndoState reply_undo;
                if (!position.make_move(reply, reply_undo)) continue;
                position.unmake_move(reply, reply_undo);
                has_legal_reply = true;
                break;
            }
        }

        position.unmake_move(move, undo);
        if (!has_legal_reply) return move;
    }
    return Move::null();
}

int Search::quiescence(Position& position, int alpha, int beta, int ply) {
    if (ply >= MaxPly - 1) return evaluator_.evaluate(position);
    ++nodes_;
    selective_depth_ = std::max(selective_depth_, ply);
    pv_length_[ply] = ply;
    if (should_stop()) return evaluator_.evaluate(position);

    const bool in_check = position.in_check(position.side_to_move());
    const int rule50 = rule50_score(position, ply, in_check);
    if (rule50 != NoScore) return rule50;
    if (position.is_repetition(2) || position.is_draw_by_material()) {
        return draw_score(position);
    }

    const int original_alpha = alpha;
    const std::uint64_t key = position.key();
    const bool rule50_sensitive = position.halfmove_clock() + 8 >= 100;

    // At shallow frontiers, a captures-only qsearch can value a move as winning
    // while overlooking an immediate quiet mate. Detect only mate-in-one here;
    // adding general quiet checks would expand quiescence without a firm bound.
    if (!in_check && ply <= QuietMateScanMaxPly) {
        const Move quiet_mate = find_quiet_mate(position, ply);
        if (!quiet_mate.is_null()) {
            const int mate = MateScore - ply - 1;
            pv_table_[ply][ply] = quiet_mate;
            pv_length_[ply] = ply + 1;
            selective_depth_ = std::max(selective_depth_, ply + 1);
            if (!rule50_sensitive) {
                store(key, 0, mate, NoScore, Bound::Exact, quiet_mate, ply);
            }
            return mate;
        }
    }

    Move tt_move = Move::null();
    int tt_static_eval = NoScore;
    int tt_score = NoScore;
    Bound tt_bound = Bound::None;
    if (TTEntry* entry = probe(key)) {
        entry->generation = generation_;
        tt_move = entry->move;
        if (!verification_search_) {
            tt_static_eval = entry->static_eval == TTNoEval ? NoScore : entry->static_eval;
            tt_score = score_from_tt(entry->score, ply);
            tt_bound = entry->bound;
            if (!rule50_sensitive && entry->depth >= 0) {
                if (entry->bound == Bound::Exact) return tt_score;
                if (entry->bound == Bound::Lower && tt_score >= beta) return tt_score;
                if (entry->bound == Bound::Upper && tt_score <= alpha) return tt_score;
            }
        }
    }

    int stand_pat = -Infinity;
    int raw_static_eval = NoScore;
    if (!in_check) {
        raw_static_eval = tt_static_eval != NoScore
            ? tt_static_eval
            : evaluator_.evaluate(position);
        stand_pat = corrected_static_eval(position, raw_static_eval);
        if (!rule50_sensitive && tt_score != NoScore &&
            std::abs(tt_score) < MateThreshold) {
            if ((tt_bound == Bound::Lower || tt_bound == Bound::Exact) &&
                tt_score > stand_pat) {
                stand_pat = tt_score;
            } else if ((tt_bound == Bound::Upper || tt_bound == Bound::Exact) &&
                       tt_score < stand_pat) {
                stand_pat = tt_score;
            }
        }
        if (stand_pat >= beta) {
            if (!rule50_sensitive) {
                store(key, 0, stand_pat, raw_static_eval, Bound::Lower,
                      Move::null(), ply);
            }
            return stand_pat;
        }
        if (stand_pat > alpha) alpha = stand_pat;
    }

    std::vector<Move>& pseudo = generated_moves_[ply];
    if (in_check) position.generate_pseudo_moves(pseudo);
    else position.generate_pseudo_captures(pseudo);

    const ContinuationRows qsearch_continuation =
        in_check ? continuation_rows(ply) : ContinuationRows{};
    score_moves(position, pseudo, ply, tt_move, !in_check,
                qsearch_continuation);

    int legal_moves = 0;
    int best = stand_pat;
    Move best_move = Move::null();
    std::vector<ScoredMove>& ordered = move_lists_[ply];
    for (std::size_t move_index = 0; move_index < ordered.size(); ++move_index) {
        const auto best_candidate = std::max_element(
            ordered.begin() + static_cast<std::ptrdiff_t>(move_index), ordered.end(),
            [](const ScoredMove& lhs, const ScoredMove& rhs) { return lhs.score < rhs.score; });
        std::iter_swap(ordered.begin() + static_cast<std::ptrdiff_t>(move_index), best_candidate);
        const Move move = ordered[move_index].move;
        const int see = !in_check ? ordered[move_index].see : 0;
        const bool delta_prunable =
            !in_check && !move.is_promotion() &&
            stand_pat + captured_value(position, move) + 180 <= alpha;
        const bool pruning_candidate =
            !in_check && !move.is_promotion() &&
            (delta_prunable || see < 0);
        if (pruning_candidate && !position.gives_check(move)) continue;

        UndoState undo;
        if (!position.make_move(move, undo)) continue;
        ++legal_moves;
        move_stack_[ply] = move;
        moved_piece_stack_[ply] = position.piece_at(move.to);
        const int score = -quiescence(position, -beta, -alpha, ply + 1);
        position.unmake_move(move, undo);

        if (should_stop()) return alpha;
        if (score > best) {
            best = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            update_pv(ply, move);
            if (alpha >= beta) {
                if (!rule50_sensitive) {
                    store(key, 0, alpha, raw_static_eval,
                          Bound::Lower, move, ply);
                }
                return alpha;
            }
        }
    }

    if (legal_moves == 0) {
        if (in_check) {
            const int mate = -MateScore + ply;
            if (!rule50_sensitive) {
                store(key, 0, mate, NoScore, Bound::Exact, Move::null(), ply);
            }
            return mate;
        }

        // Captures-only quiescence can otherwise mistake stalemate for a large
        // static advantage. This scan is delayed until no tactical move was
        // searched, and normally exits after the first legal quiet move.
        position.generate_pseudo_moves(pseudo);
        bool has_legal_move = false;
        for (const Move& move : pseudo) {
            UndoState undo;
            if (!position.make_move(move, undo)) continue;
            position.unmake_move(move, undo);
            has_legal_move = true;
            break;
        }
        if (!has_legal_move) {
            const int draw = draw_score(position);
            if (!rule50_sensitive) {
                store(key, 0, draw, raw_static_eval, Bound::Exact,
                      Move::null(), ply);
            }
            return draw;
        }
    }

    const int result = best == -Infinity ? alpha : best;
    if (!rule50_sensitive) {
        const Bound bound = result > original_alpha ? Bound::Exact : Bound::Upper;
        store(key, 0, result, raw_static_eval, bound, best_move, ply);
    }
    return result;
}

int Search::alpha_beta(Position& position, int depth, int alpha, int beta, int ply,
                       bool pv_node, bool cut_node, bool allow_null,
                       const Move& previous_move) {
    if (ply >= MaxPly - 1) return evaluator_.evaluate(position);
    ++nodes_;
    selective_depth_ = std::max(selective_depth_, ply);
    pv_length_[ply] = ply;
    if (should_stop()) return evaluator_.evaluate(position);

    alpha = std::max(alpha, -MateScore + ply);
    beta = std::min(beta, MateScore - ply - 1);
    if (alpha >= beta) return alpha;

    const bool in_check = position.in_check(position.side_to_move());
    if (ply > 0) {
        const int rule50 = rule50_score(position, ply, in_check);
        if (rule50 != NoScore) return rule50;
        if (position.is_repetition(2) || position.is_draw_by_material()) {
            return draw_score(position);
        }
    }
    if (depth <= 0) return quiescence(position, alpha, beta, ply);
    if (in_check) ++depth;

    const int original_alpha = alpha;
    const std::uint64_t key = position.key();
    Move tt_move = Move::null();
    int tt_static_eval = NoScore;
    int tt_score = NoScore;
    Bound tt_bound = Bound::None;
    const bool rule50_sensitive = position.halfmove_clock() + depth >= 100;
    if (TTEntry* entry = probe(key)) {
        entry->generation = generation_;
        tt_move = entry->move;
        if (!verification_search_) {
            tt_static_eval = entry->static_eval == TTNoEval ? NoScore : entry->static_eval;
            tt_score = score_from_tt(entry->score, ply);
            tt_bound = entry->bound;
            if (!rule50_sensitive && entry->depth >= depth) {
                if (entry->bound == Bound::Exact) return tt_score;
                if (!pv_node && entry->bound == Bound::Lower && tt_score >= beta) return tt_score;
                if (!pv_node && entry->bound == Bound::Upper && tt_score <= alpha) return tt_score;
            }
        }
    }

    const int raw_static_eval = in_check ? NoScore :
        (tt_static_eval != NoScore ? tt_static_eval : evaluator_.evaluate(position));
    const int static_eval = in_check ? NoScore :
        corrected_static_eval(position, raw_static_eval);
    int pruning_eval = static_eval;
    if (!in_check && !rule50_sensitive && tt_score != NoScore &&
        std::abs(tt_score) < MateThreshold) {
        if ((tt_bound == Bound::Lower || tt_bound == Bound::Exact) &&
            tt_score > pruning_eval) {
            pruning_eval = tt_score;
        } else if ((tt_bound == Bound::Upper || tt_bound == Bound::Exact) &&
                   tt_score < pruning_eval) {
            pruning_eval = tt_score;
        }
    }
    eval_stack_[ply] = static_eval;
    const bool improving = !in_check && ply >= 2 && eval_stack_[ply - 2] != NoScore &&
                           static_eval > eval_stack_[ply - 2];

    if (!pv_node && !in_check && std::abs(beta) < MateThreshold) {
        // Razoring: a very low static score at shallow depth is unlikely to recover
        // without a tactical move, which quiescence will still examine.
        if (depth <= 2 && pruning_eval + 180 + depth * 110 <= alpha) {
            const int razor = quiescence(position, alpha, beta, ply);
            if (search_aborted()) return alpha;
            if (razor <= alpha) return razor;
        }

        // Reverse futility pruning.
        const int rfp_margin = (improving ? 70 : 95) * depth;
        if (depth <= 6 && pruning_eval - rfp_margin >= beta) {
            return pruning_eval - rfp_margin;
        }
    }

    if (allow_null && !pv_node && !in_check && depth >= 3 &&
        pruning_eval >= beta && position.has_non_pawn_material(position.side_to_move()) &&
        std::abs(beta) < MateThreshold) {
        const int extra = std::clamp((pruning_eval - beta) / 180, 0, 3);
        const int reduction = std::min(depth - 1, 3 + depth / 4 + extra);
        UndoState undo;
        position.make_null_move(undo);
        move_stack_[ply] = Move::null();
        moved_piece_stack_[ply] = Empty;
        const int null_score = -alpha_beta(position, depth - reduction, -beta, -beta + 1,
                                           ply + 1, false, !cut_node, false, Move::null());
        position.unmake_null_move(undo);
        if (should_stop()) return alpha;
        if (null_score >= beta) {
            if (depth < 9) return null_score;
            const int verification = alpha_beta(position, depth - reduction, beta - 1, beta,
                                                ply, false, false, false, previous_move);
            if (search_aborted()) return alpha;
            if (verification >= beta) return null_score;
        }
    }

    // ProbCut tests forcing captures against a raised beta before searching the
    // full move list. Requiring a non-losing exchange and a confirming reduced
    // search keeps the pruning conservative in tactically unstable positions.
    if (!pv_node && !in_check && depth >= 6 && std::abs(beta) < MateThreshold) {
        constexpr int ProbCutMargin = 170;
        const int probcut_beta = std::min(MateThreshold - 1, beta + ProbCutMargin);
        std::vector<Move>& tactical = generated_moves_[ply];
        position.generate_pseudo_captures(tactical);
        score_moves(position, tactical, ply, tt_move, true,
                    ContinuationRows{});

        std::vector<ScoredMove>& ordered = move_lists_[ply];
        for (std::size_t move_index = 0; move_index < ordered.size(); ++move_index) {
            const auto best_candidate = std::max_element(
                ordered.begin() + static_cast<std::ptrdiff_t>(move_index), ordered.end(),
                [](const ScoredMove& lhs, const ScoredMove& rhs) { return lhs.score < rhs.score; });
            std::iter_swap(ordered.begin() + static_cast<std::ptrdiff_t>(move_index), best_candidate);
            const Move move = ordered[move_index].move;
            const int see_threshold = std::max(0, probcut_beta - pruning_eval - 120);
            if (!move.is_promotion() && ordered[move_index].see < see_threshold) {
                continue;
            }

            UndoState undo;
            if (!position.make_move(move, undo)) continue;
            move_stack_[ply] = move;
            moved_piece_stack_[ply] = position.piece_at(move.to);
            int score = -quiescence(position, -probcut_beta, -probcut_beta + 1, ply + 1);
            if (!search_aborted() && score >= probcut_beta) {
                score = -alpha_beta(position, depth - 4, -probcut_beta,
                                    -probcut_beta + 1, ply + 1,
                                    false, true, true, move);
            }
            position.unmake_move(move, undo);

            if (should_stop()) return alpha;
            if (score >= probcut_beta) {
                if (!rule50_sensitive) {
                    store(key, depth - 3, score, raw_static_eval,
                          Bound::Lower, move, ply);
                }
                return score;
            }
        }
    }

    // Internal iterative reduction when the hash table has no useful move.
    if (tt_move.is_null() && depth >= 6 && pv_node) --depth;

    std::vector<Move>& pseudo = generated_moves_[ply];
    position.generate_pseudo_moves(pseudo);
    const ContinuationRows node_continuation = continuation_rows(ply);
    score_moves(position, pseudo, ply, tt_move, false, node_continuation);

    int legal_moves = 0;
    int quiet_moves = 0;
    int best_score = -Infinity;
    Move best_move = Move::null();
    std::vector<Move>& tried_quiets = tried_quiets_[ply];
    std::vector<Move>& tried_captures = tried_captures_[ply];
    tried_quiets.clear();
    tried_captures.clear();

    const Color us = position.side_to_move();
    std::vector<ScoredMove>& ordered = move_lists_[ply];
    for (std::size_t move_index = 0; move_index < ordered.size(); ++move_index) {
        const auto best_candidate = std::max_element(
            ordered.begin() + static_cast<std::ptrdiff_t>(move_index), ordered.end(),
            [](const ScoredMove& lhs, const ScoredMove& rhs) { return lhs.score < rhs.score; });
        std::iter_swap(ordered.begin() + static_cast<std::ptrdiff_t>(move_index), best_candidate);
        const Move move = ordered[move_index].move;
        const bool quiet = is_quiet(move);
        const int see = quiet ? 0 : ordered[move_index].see;
        const int history_score = quiet
            ? history_[us][move.from][move.to] +
                  continuation_score(position, move, node_continuation)
            : 0;

        UndoState undo;
        if (!position.make_move(move, undo)) continue;
        ++legal_moves;
        if (quiet) ++quiet_moves;
        move_stack_[ply] = move;
        moved_piece_stack_[ply] = position.piece_at(move.to);

        const bool gives_check = position.in_check(position.side_to_move());

        bool prune = false;
        if (!pv_node && !in_check && !gives_check && best_score > -MateThreshold) {
            if (quiet) {
                const int lmp_limit = 3 + depth * depth + (improving ? 3 : 0);
                if (depth <= 4 && quiet_moves > lmp_limit) prune = true;

                const int futility_margin = 80 + depth * (improving ? 70 : 95);
                if (depth <= 5 && legal_moves > 1 && pruning_eval + futility_margin <= alpha &&
                    history_score < 6000) {
                    prune = true;
                }
            } else if (depth <= 4 && legal_moves > 1 && see < -90 * depth) {
                prune = true;
            }
        }
        if (prune) {
            position.unmake_move(move, undo);
            continue;
        }

        // Check extension is applied once on entry to the checked node. Applying
        // another extension to the checking move here would preserve full depth
        // across every check/evasion pair and allow pathological king chases.
        const int new_depth = depth - 1;
        int score;

        if (legal_moves == 1) {
            score = -alpha_beta(position, new_depth, -beta, -alpha, ply + 1,
                                pv_node, false, true, move);
        } else {
            int reduction = 0;
            if (!gives_check && depth >= 3 && legal_moves >= 3) {
                if (quiet) {
                    reduction = lmr_reduction(depth, legal_moves, pv_node, improving);
                    if (cut_node) ++reduction;
                    if (history_score > 5000) --reduction;
                    if (history_score < -5000) ++reduction;
                } else if (see < 0 && depth >= 4) {
                    reduction = 1 + (see < -250 ? 1 : 0);
                }
                reduction = std::clamp(reduction, 0, std::max(0, new_depth - 1));
            }

            score = -alpha_beta(position, new_depth - reduction, -alpha - 1, -alpha,
                                ply + 1, false, true, true, move);
            if (!search_aborted() && score > alpha && reduction > 0) {
                score = -alpha_beta(position, new_depth, -alpha - 1, -alpha,
                                    ply + 1, false, !cut_node, true, move);
            }
            if (!search_aborted() && score > alpha && score < beta) {
                score = -alpha_beta(position, new_depth, -beta, -alpha,
                                    ply + 1, pv_node, false, true, move);
            }
        }

        position.unmake_move(move, undo);
        if (should_stop()) return alpha;

        if (quiet) tried_quiets.push_back(move);
        else if (move.is_capture()) tried_captures.push_back(move);
        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
            update_pv(ply, move);
            if (alpha >= beta) {
                if (quiet) {
                    update_quiet_history(position, us, move, depth, tried_quiets, ply);
                    if (!previous_move.is_null()) {
                        const Piece prior_piece = position.piece_at(previous_move.to);
                        if (prior_piece != Empty) {
                            counter_moves_[prior_piece][previous_move.to] = move;
                        }
                    }
                } else if (move.is_capture()) {
                    update_capture_history(position, move, depth, tried_captures);
                }
                if (!rule50_sensitive) {
                    store(key, depth, alpha, raw_static_eval, Bound::Lower, move, ply);
                }
                if (quiet) {
                    update_correction_history(position, depth, raw_static_eval,
                                              alpha, Bound::Lower);
                }
                return alpha;
            }
        }
    }

    if (legal_moves == 0) return in_check ? -MateScore + ply : draw_score(position);

    const Bound bound = best_score > original_alpha ? Bound::Exact : Bound::Upper;
    if (!rule50_sensitive) {
        store(key, depth, best_score, raw_static_eval, bound, best_move, ply);
    }
    if (best_move.is_null() || is_quiet(best_move)) {
        update_correction_history(position, depth, raw_static_eval, best_score, bound);
    }
    return best_score;
}

int Search::search_root(Position& position, std::vector<RootMove>& root_moves,
                        int depth, int alpha, int beta) {
    const int original_alpha = alpha;
    int best_score = -Infinity;
    int searched = 0;

    std::stable_sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs,
                                                              const RootMove& rhs) {
        if (lhs.previous_score != rhs.previous_score) return lhs.previous_score > rhs.previous_score;
        return lhs.policy > rhs.policy;
    });

    for (RootMove& root_move : root_moves) {
        if (should_stop(true)) break;
        UndoState undo;
        if (!position.make_move(root_move.move, undo)) continue;
        ++searched;
        move_stack_[0] = root_move.move;
        moved_piece_stack_[0] = position.piece_at(root_move.move.to);

        int score;
        bool exact_score = searched == 1;
        if (searched == 1) {
            score = -alpha_beta(position, depth - 1, -beta, -alpha, 1,
                                true, false, true, root_move.move);
        } else {
            score = -alpha_beta(position, depth - 1, -alpha - 1, -alpha, 1,
                                false, true, true, root_move.move);
            if (!search_aborted() && score > alpha && score < beta) {
                score = -alpha_beta(position, depth - 1, -beta, -alpha, 1,
                                    true, false, true, root_move.move);
                exact_score = true;
            }
        }

        position.unmake_move(root_move.move, undo);
        if (search_aborted()) break;

        root_move.score = score;
        root_move.exact = exact_score;
        root_move.pv.clear();
        root_move.pv.push_back(root_move.move);
        const std::vector<Move> child_pv = current_pv(1);
        root_move.pv.insert(root_move.pv.end(), child_pv.begin(), child_pv.end());

        if (score > best_score) best_score = score;
        if (score > alpha) {
            alpha = score;
            pv_table_[0][0] = root_move.move;
            const int child_length = std::clamp(pv_length_[1], 1, MaxPly);
            for (int index = 1; index < child_length; ++index) {
                pv_table_[0][index] = pv_table_[1][index];
            }
            pv_length_[0] = child_length;
            if (alpha >= beta) break;
        }
    }

    if (searched == 0) return original_alpha;
    std::stable_sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs,
                                                              const RootMove& rhs) {
        return lhs.score > rhs.score;
    });
    return best_score;
}

void Search::add_book_line(const std::vector<std::string>& moves, int weight) {
    if (weight <= 0) return;

    Position position;
    for (const std::string& text : moves) {
        const Move move = position.parse_uci_move(text);
        if (move.is_null()) return;
        auto& entries = book_[position.key()];
        auto found = std::find_if(entries.begin(), entries.end(), [&](const BookMove& entry) {
            return entry.move == move;
        });
        if (found == entries.end()) {
            entries.push_back(BookMove{move, weight});
        } else {
            const std::int64_t combined =
                static_cast<std::int64_t>(found->weight) + weight;
            found->weight = static_cast<int>(std::min<std::int64_t>(
                combined, std::numeric_limits<int>::max()));
        }

        UndoState undo;
        if (!position.make_move(move, undo)) return;
    }
}

void Search::build_builtin_book() {
    book_.clear();
    add_book_line({"e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6",
                   "b5a4", "g8f6", "e1g1", "f8e7"}, 120);
    add_book_line({"e2e4", "c7c5", "g1f3", "d7d6", "d2d4", "c5d4",
                   "f3d4", "g8f6", "b1c3", "a7a6"}, 110);
    add_book_line({"e2e4", "e7e6", "d2d4", "d7d5", "b1c3", "g8f6",
                   "e4e5", "f6d7"}, 85);
    add_book_line({"d2d4", "d7d5", "c2c4", "e7e6", "b1c3", "g8f6",
                   "g1f3", "f8e7"}, 110);
    add_book_line({"d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "f8g7",
                   "e2e4", "d7d6"}, 105);
    add_book_line({"c2c4", "e7e5", "b1c3", "g8f6", "g2g3", "d7d5",
                   "c4d5", "f6d5"}, 70);
    add_book_line({"g1f3", "d7d5", "d2d4", "g8f6", "c2c4", "e7e6"}, 55);

    if (!options_.book_file.empty()) {
        if (!load_book_file(options_.book_file) && options_.book_file == "openings/book_lines.txt") {
            if (!load_book_file("../openings/book_lines.txt")) {
                load_book_file("../../openings/book_lines.txt");
            }
        }
    }
}

bool Search::load_book_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) return false;

    std::string line;
    while (std::getline(input, line)) {
        const std::size_t comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        const std::size_t separator = line.find('|');
        if (separator == std::string::npos) continue;

        int weight = 0;
        std::istringstream weight_stream(line.substr(0, separator));
        if (!(weight_stream >> weight) || weight <= 0) continue;

        std::istringstream move_stream(line.substr(separator + 1));
        std::vector<std::string> moves;
        for (std::string move; move_stream >> move;) moves.push_back(move);
        if (!moves.empty()) add_book_line(moves, weight);
    }
    return true;
}

Move Search::select_book_move(const Position& position) {
    const auto found = book_.find(position.key());
    if (found == book_.end() || found->second.empty()) return Move::null();

    std::vector<BookMove> legal_entries;
    legal_entries.reserve(found->second.size());
    for (const BookMove& entry : found->second) {
        UndoState undo;
        Position copy = position;
        if (copy.make_move(entry.move, undo)) legal_entries.push_back(entry);
    }
    if (legal_entries.empty()) return Move::null();

    if (options_.book_randomness <= 0 || legal_entries.size() == 1) {
        return std::max_element(legal_entries.begin(), legal_entries.end(),
            [](const BookMove& lhs, const BookMove& rhs) { return lhs.weight < rhs.weight; })->move;
    }

    const int randomness = std::clamp(options_.book_randomness, 1, 100);
    const double exponent = static_cast<double>(400 - 3 * randomness) / 100.0;
    const auto sampling_weight = [exponent](const BookMove& entry) {
        return std::pow(static_cast<double>(std::max(1, entry.weight)), exponent);
    };

    double total_weight = 0.0;
    for (const BookMove& entry : legal_entries) {
        total_weight += sampling_weight(entry);
    }
    if (!std::isfinite(total_weight) || total_weight <= 0.0) {
        return std::max_element(legal_entries.begin(), legal_entries.end(),
            [](const BookMove& lhs, const BookMove& rhs) {
                return lhs.weight < rhs.weight;
            })->move;
    }

    // Use the top 53 generator bits to build an exactly specified [0, 1)
    // double. This avoids implementation-defined distribution behavior and the
    // GCC 14 false-positive emitted by std::discrete_distribution's temporary
    // allocation while retaining the same weighted-book semantics.
    constexpr double InverseTwoTo53 = 1.0 / 9007199254740992.0;
    double target = static_cast<double>(random_() >> 11U) *
                    InverseTwoTo53 * total_weight;
    for (const BookMove& entry : legal_entries) {
        const double weight = sampling_weight(entry);
        if (target < weight) return entry.move;
        target -= weight;
    }
    return legal_entries.back().move;
}

std::optional<SearchResult> Search::confirm_human_candidate(
    const Position& position, const Move& candidate, int completed_depth) {
    if (candidate.is_null() || completed_depth <= 0 || should_stop(true)) {
        return std::nullopt;
    }

    EngineOptions verifier_options = options_;
    verifier_options.hash_mb = 1;
    verifier_options.use_book = false;
    verifier_options.uci_limit_strength = false;
    verifier_options.human_style = false;
    verifier_options.human_skill = 20;

    Search verifier(evaluator_, verifier_options);
    if (should_stop(true)) return std::nullopt;
    verifier.set_options(verifier_options);
    if (should_stop(true)) return std::nullopt;
    SearchLimits verifier_limits;
    verifier_limits.depth = completed_depth;
    verifier_limits.search_moves_specified = true;
    verifier_limits.search_moves.push_back(candidate);
    verifier_limits.external_stops.reserve(limits_.external_stops.size() + 1);
    verifier_limits.external_stops.push_back(&stop_requested_);
    verifier_limits.external_stops.insert(
        verifier_limits.external_stops.end(),
        limits_.external_stops.begin(),
        limits_.external_stops.end());
    std::optional<std::chrono::steady_clock::time_point> verifier_deadline;
    if (has_hard_deadline_) verifier_deadline = hard_deadline_;
    if (limits_.external_deadline != nullptr &&
        (!verifier_deadline.has_value() ||
         *limits_.external_deadline < *verifier_deadline)) {
        verifier_deadline = *limits_.external_deadline;
    }
    if (verifier_deadline.has_value()) {
        verifier_limits.external_deadline = &*verifier_deadline;
    }
    std::uint64_t verifier_node_budget = 0;
    if (limits_.node_limit != 0) {
        verifier_node_budget = limits_.node_limit -
            std::min(nodes_, limits_.node_limit);
        if (verifier_node_budget == 0) return std::nullopt;
        verifier_limits.node_limit = verifier_node_budget;
    }

    SearchResult result = verifier.think(position, verifier_limits);
    const bool verifier_overran_parent = verifier_node_budget != 0 &&
        result.nodes > verifier_node_budget;
    nodes_ += verifier_overran_parent ? verifier_node_budget : result.nodes;
    selective_depth_ = std::max(selective_depth_, result.selective_depth);
    if (verifier_overran_parent || should_stop(true) ||
        result.depth != completed_depth ||
        result.best != candidate) {
        return std::nullopt;
    }
    return result;
}

Move Search::select_human_move(const Position& position, std::vector<RootMove>& root_moves,
                               int completed_depth) {
    if (root_moves.empty()) return Move::null();
    std::stable_sort(root_moves.begin(), root_moves.end(), [](const RootMove& lhs,
                                                              const RootMove& rhs) {
        return lhs.score > rhs.score;
    });
    const HumanSettings human = resolved_human_settings(options_);
    if (!human.enabled || !selection_opportunity_ || root_moves.size() == 1 ||
        completed_depth < 2 ||
        std::abs(root_moves.front().score) >= MateThreshold || should_stop(true)) {
        // Never randomise a proven mate (or a forced-mate defence). Root PVS
        // bounds for unsearched alternatives are not reliable enough to safely
        // distinguish equivalent mate lines here.
        return root_moves.front().move;
    }
    if (limits_.ponder && !ponder_time_activated_) {
        // A bounded ponder can finish before ponderhit. Without an activated
        // clock, a nested confirmation would have no deadline to inherit.
        return root_moves.front().move;
    }

    const int skill = human.skill;
    const int skill_gap = 20 - skill;
    const int allowance = human_loss_allowance(human);
    const int best_score = root_moves.front().score;
    const int threshold = best_score - allowance;
    const double temperature = std::max(3.0, 4.0 + skill_gap * 3.5);
    const std::size_t candidate_limit = std::min<std::size_t>(
        root_moves.size(), static_cast<std::size_t>(6 + skill_gap));

    // PVS and aspiration windows can leave bounds on every non-best root move.
    // Use a cheap narrow search to filter the weighted pool. Any sampled
    // alternative is then confirmed by a fresh restricted iterative search
    // before it can be returned.
    std::vector<bool> eligible(candidate_limit, false);
    eligible.front() = true;
    if (selection_budget_reserved_) {
        // Root PVS scores are provisional here. Sample from plausible moves and
        // spend the reserved budget on the one fresh search that can admit it.
        for (std::size_t index = 1; index < candidate_limit; ++index) {
            const RootMove& candidate = root_moves[index];
            eligible[index] = candidate.score != NoScore && candidate.score >= threshold;
        }
    } else {
        for (std::size_t index = 1; index < candidate_limit; ++index) {
            RootMove& candidate = root_moves[index];
            if (eligible[index] || candidate.score == NoScore ||
                candidate.score < threshold) {
                continue;
            }
            if (should_stop(true)) break;

            Position copy = position;
            UndoState undo;
            if (!copy.make_move(candidate.move, undo)) {
                candidate.score = NoScore;
                continue;
            }
            move_stack_[0] = candidate.move;
            moved_piece_stack_[0] = copy.piece_at(candidate.move.to);
            const int prior_score = candidate.score;
            verification_search_ = true;
            const int verified = -alpha_beta(copy, completed_depth - 1,
                                             -threshold, -threshold + 1, 1,
                                             true, false, true, candidate.move);
            verification_search_ = false;
            if (search_aborted()) break;
            if (verified < threshold) {
                candidate.score = verified;
                continue;
            }
            candidate.score = std::max(prior_score, threshold);
            eligible[index] = true;
        }
    }

    std::vector<std::size_t> candidates;
    std::vector<double> weights;
    for (std::size_t index = 0; index < candidate_limit; ++index) {
        const RootMove& move = root_moves[index];
        if (!eligible[index]) continue;
        candidates.push_back(index);
        const int weighted_score = std::min(best_score, move.score);
        const double eval_term = std::exp((weighted_score - best_score) / temperature);
        const double policy_term = std::exp(static_cast<double>(move.policy) * 0.20);
        const double rank_term = 1.0 / (1.0 + static_cast<double>(index) * 0.18);
        const double best_bias = index == 0 ? 1.0 + static_cast<double>(skill) * 0.55 : 1.0;
        weights.push_back(eval_term * policy_term * rank_term * best_bias);
    }
    if (candidates.size() <= 1) return root_moves.front().move;

    const int max_confirmations = selection_budget_reserved_ ? 1 : 3;
    for (int attempt = 0;
         attempt < max_confirmations && candidates.size() > 1;
         ++attempt) {
        std::discrete_distribution<std::size_t> distribution(
            weights.begin(), weights.end());
        const std::size_t slot = distribution(random_);
        const std::size_t index = candidates[slot];
        if (index == 0) return root_moves.front().move;

        const std::optional<SearchResult> confirmed = confirm_human_candidate(
            position, root_moves[index].move, completed_depth);
        if (!confirmed.has_value()) return root_moves.front().move;
        root_moves[index].score = confirmed->score_cp;
        root_moves[index].exact = true;
        root_moves[index].pv = confirmed->pv;
        if (confirmed->score_cp >= threshold) return root_moves[index].move;

        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(slot));
        weights.erase(weights.begin() + static_cast<std::ptrdiff_t>(slot));
    }
    return root_moves.front().move;
}

SearchResult Search::think(Position position, const SearchLimits& limits,
                           const InfoCallback& callback,
                           const StartCallback& start_callback) {
    searching_.store(true, std::memory_order_relaxed);
    struct SearchingGuard {
        std::atomic<bool>& flag;
        ~SearchingGuard() { flag.store(false, std::memory_order_relaxed); }
    } guard{searching_};

    stop_requested_.store(false, std::memory_order_relaxed);
    if (start_callback) start_callback();
    limits_ = limits;
    main_node_limit_ = 0;
    main_budget_exhausted_ = false;
    selection_budget_reserved_ = false;
    selection_opportunity_ = false;
    const HumanSettings active_human = resolved_human_settings(options_);
    selection_opportunity_ = human_selection_opportunity(active_human, random_);
    main_phase_ = selection_opportunity_ && human_loss_allowance(active_human) > 0;
    if (main_phase_ && limits_.node_limit != 0) {
        main_node_limit_ = std::max<std::uint64_t>(1, limits_.node_limit / 2);
    }
    if (!limits_.ponder) {
        ponder_state_.store(PonderState::Inactive, std::memory_order_release);
    } else if (ponder_state_.load(std::memory_order_acquire) == PonderState::Inactive) {
        // Direct Search users do not need to call begin_ponder(). UCI calls it
        // before launching the worker so an immediate ponderhit cannot be lost.
        begin_ponder();
    }
    root_side_ = position.side_to_move();
    if (limits_.depth <= 0) limits_.depth = MaxPly - 2;
    limits_.depth = std::clamp(limits_.depth, 1, MaxPly - 2);
    nodes_ = 0;
    selective_depth_ = 0;
    start_time_ = std::chrono::steady_clock::now();
    configure_time(position);
    selection_budget_reserved_ = main_phase_ &&
        (main_node_limit_ != 0 || hard_time_budget_ms_ > 0);
    main_phase_ = selection_budget_reserved_;
    // configure_time() may have created this from the provisional main phase.
    if (!selection_budget_reserved_) has_main_deadline_ = false;
    ++generation_;
    if (generation_ == 0) ++generation_;

    for (auto& length : pv_length_) length = 0;
    for (auto& row : pv_table_) row.fill(Move::null());
    eval_stack_.fill(NoScore);
    move_stack_.fill(Move::null());
    moved_piece_stack_.fill(Empty);

    SearchResult result;
    std::vector<Move> legal_moves;
    position.generate_legal_moves(legal_moves);
    if (limits_.search_moves_specified) {
        legal_moves.erase(std::remove_if(legal_moves.begin(), legal_moves.end(), [&](const Move& move) {
            return std::find(limits_.search_moves.begin(), limits_.search_moves.end(), move) ==
                   limits_.search_moves.end();
        }), legal_moves.end());
    }
    if (legal_moves.empty()) {
        result.score_cp = limits_.search_moves_specified ? 0 :
            (position.in_check(position.side_to_move()) ? -MateScore : 0);
        result.time_ms = elapsed_ms();
        return result;
    }

    const bool timed_play = limits_.movetime_ms > 0 || limits_.white_time_ms > 0 ||
                            limits_.black_time_ms > 0;
    const bool human_limited = resolved_human_settings(options_).enabled;
    if (options_.use_book && !human_limited && timed_play &&
        position.fullmove_number() <= 12 &&
        limits_.node_limit == 0 && !limits_.infinite && !limits_.search_moves_specified) {
        const Move book_move = select_book_move(position);
        if (!book_move.is_null()) {
            result.best = book_move;
            result.pv = {book_move};
            result.time_ms = elapsed_ms();
            return result;
        }
    }

    const std::vector<float> policies = evaluator_.policy_scores(position, legal_moves);
    std::vector<RootMove> root_moves;
    root_moves.reserve(legal_moves.size());
    for (std::size_t index = 0; index < legal_moves.size(); ++index) {
        root_moves.push_back(RootMove{legal_moves[index], NoScore, NoScore,
                                     index < policies.size() ? policies[index] : 0.0F, false, {}});
    }

    // Seed root ordering with the transposition move, then policy/history.
    Move root_tt_move = Move::null();
    if (const TTEntry* entry = probe(position.key())) root_tt_move = entry->move;
    for (RootMove& move : root_moves) {
        int seed = static_cast<int>(move.policy * RootPolicyScale);
        seed += history_[position.side_to_move()][move.move.from][move.move.to];
        if (same_move(move.move, root_tt_move)) seed += 2'000'000;
        move.previous_score = seed;
    }

    int previous_score = 0;
    int completed_depth = 0;
    std::vector<RootMove> completed_root_moves;
    int stable_best_iterations = 0;
    Move previous_best = Move::null();
    std::vector<Move> completed_pv;

    for (int depth = 1; depth <= limits_.depth; ++depth) {
        if (should_stop(true)) break;
        for (RootMove& move : root_moves) {
            move.previous_score = move.score == NoScore ? move.previous_score : move.score;
            move.score = NoScore;
            move.exact = false;
            move.pv.clear();
        }

        int delta = depth >= 5 ? 18 : Infinity;
        int alpha = depth >= 5 ? std::max(-Infinity, previous_score - delta) : -Infinity;
        int beta = depth >= 5 ? std::min(Infinity, previous_score + delta) : Infinity;
        int score = previous_score;
        bool iteration_complete = false;

        while (!should_stop(true)) {
            pv_length_[0] = 0;
            score = search_root(position, root_moves, depth, alpha, beta);
            if (search_aborted()) break;

            if (score <= alpha) {
                delta = std::min(4096, delta * 2);
                alpha = std::max(-Infinity, score - delta);
                beta = std::min(Infinity, score + delta / 2);
                continue;
            }
            if (score >= beta) {
                delta = std::min(4096, delta * 2);
                beta = std::min(Infinity, score + delta);
                alpha = std::max(-Infinity, score - delta / 2);
                continue;
            }
            iteration_complete = true;
            break;
        }

        if (!iteration_complete || root_moves.empty() || root_moves.front().score == NoScore) break;

        completed_depth = depth;
        completed_root_moves = root_moves;
        previous_score = root_moves.front().score;
        completed_pv = root_moves.front().pv;
        if (completed_pv.empty()) completed_pv = current_pv(0);

        if (root_moves.front().move == previous_best) ++stable_best_iterations;
        else stable_best_iterations = 0;
        previous_best = root_moves.front().move;

        if (callback) {
            const int time = std::max(1, elapsed_ms());
            callback(SearchInfo{depth, selective_depth_, previous_score, nodes_,
                                nodes_ * 1000ULL / static_cast<std::uint64_t>(time),
                                time, hashfull(), completed_pv});
        }

        if (limits_.node_limit != 0 && nodes_ >= limits_.node_limit) break;
        if (soft_time_expired() && depth >= 2) break;
        if (has_soft_deadline_ && stable_best_iterations >= 3 && depth >= 6) {
            const auto now = std::chrono::steady_clock::now();
            const auto total = soft_deadline_ - start_time_;
            if (now - start_time_ >= total * 3 / 4) break;
        }
    }

    main_phase_ = false;
    main_budget_exhausted_ = false;

    if (completed_depth == 0) {
        // A stop can arrive before depth one finishes. Root moves are already
        // ordered by the TT move, policy and history (and possibly by a few
        // completed partial scores), so preserve that information instead of
        // falling back to move-generator order.
        result.best = root_moves.empty() ? legal_moves.front() : root_moves.front().move;
        result.score_cp = 0;
        result.pv = {result.best};
    } else {
        // A deeper iteration may have been interrupted after overwriting only a
        // subset of root scores. Final move selection must use the last complete
        // iteration, not those partial bounds.
        result.best = select_human_move(position, completed_root_moves, completed_depth);
        const auto chosen = std::find_if(completed_root_moves.begin(), completed_root_moves.end(),
            [&](const RootMove& move) { return move.move == result.best; });
        if (chosen != completed_root_moves.end()) {
            result.score_cp = chosen->score;
            result.pv = chosen->pv.empty() ? std::vector<Move>{chosen->move} : chosen->pv;
        } else {
            result.score_cp = previous_score;
            result.pv = completed_pv;
        }
    }

    result.depth = completed_depth;
    result.selective_depth = selective_depth_;
    result.nodes = nodes_;
    result.time_ms = elapsed_ms();
    if (result.pv.size() >= 2) result.ponder = result.pv[1];
    return result;
}

}  // namespace proton
