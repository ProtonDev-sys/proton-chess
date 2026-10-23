#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "engine/human_prior.h"
#include "engine/search.h"

namespace proton {

struct SearchTestAccess {
    using RootMove = Search::RootMove;

    static std::vector<RootMove> roots(Search& search, Position position,
                                       const std::vector<Move>* allowed = nullptr) {
        std::vector<Move> legal;
        position.generate_legal_moves(legal);
        if (allowed != nullptr) {
            std::erase_if(legal, [&](const Move& move) {
                return std::find(allowed->begin(), allowed->end(), move) == allowed->end();
            });
        }
        const auto policies = search.evaluator_.policy_scores(position, legal);
        std::vector<RootMove> result;
        for (std::size_t index = 0; index < legal.size(); ++index) {
            result.push_back({legal[index], 17, 23, policies[index], false, {legal[index]}});
        }
        return result;
    }

    static void apply(Search& search, const Position& position,
                      std::vector<RootMove>& roots) {
        search.apply_human_book_priors(position, roots);
    }

    static void opportunity(Search& search, bool enabled) {
        search.selection_opportunity_ = enabled;
    }

    static void book(Search& search, const Position& position,
                     const std::vector<std::pair<Move, int>>& entries) {
        auto& book = search.book_[position.key()];
        book.clear();
        for (const auto& [move, weight] : entries) book.push_back({move, weight});
    }

    static std::mt19937_64 random(const Search& search) { return search.random_; }

    static void prepare_selection(Search& search, std::uint64_t seed,
                                  std::uint64_t node_limit = 0,
                                  std::uint64_t used = 0) {
        search.limits_ = {};
        search.limits_.node_limit = node_limit;
        search.nodes_ = used;
        search.stop_requested_.store(false);
        search.main_phase_ = false;
        search.main_budget_exhausted_ = false;
        search.has_main_deadline_ = false;
        search.has_hard_deadline_ = false;
        search.selection_opportunity_ = true;
        search.selection_budget_reserved_ = node_limit != 0;
        search.start_time_ = std::chrono::steady_clock::now();
        search.random_.seed(seed);
    }

    static Move select(Search& search, const Position& position,
                        std::vector<RootMove>& roots, int depth) {
        return search.select_human_move(position, roots, depth);
    }

    static std::uint64_t nodes(const Search& search) { return search.nodes_; }
    static constexpr int no_score() { return Search::NoScore; }
};

}  // namespace proton

namespace {

using Access = proton::SearchTestAccess;
using Roots = std::vector<Access::RootMove>;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

proton::EngineOptions human_options() {
    proton::EngineOptions options;
    options.hash_mb = 1;
    options.book_file.clear();
    options.use_book = true;
    options.human_style = true;
    options.human_skill = 16;
    options.human_max_loss_cp = 12;
    options.human_variety_percent = 100;
    options.book_randomness = 70;
    options.human_seed = 27;
    return options;
}

proton::Position position_after(const std::vector<std::string>& moves) {
    proton::Position position;
    for (const auto& text : moves) {
        const auto move = position.parse_uci_move(text);
        proton::UndoState undo;
        expect(!move.is_null() && position.make_move(move, undo), "fixture move: " + text);
    }
    return position;
}

const Access::RootMove& root_for(const Roots& roots, const proton::Move& move) {
    const auto found = std::find_if(roots.begin(), roots.end(), [&](const auto& root) {
        return root.move == move;
    });
    if (found == roots.end()) {
        expect(false, "expected root is present: " + move.to_uci());
        static const Access::RootMove missing;
        return missing;
    }
    return *found;
}

bool same_result(const proton::SearchResult& lhs, const proton::SearchResult& rhs) {
    return lhs.best == rhs.best && lhs.ponder == rhs.ponder && lhs.pv == rhs.pv &&
           lhs.score_cp == rhs.score_cp && lhs.depth == rhs.depth &&
           lhs.selective_depth == rhs.selective_depth && lhs.nodes == rhs.nodes;
}

proton::SearchResult run(const proton::EngineOptions& options,
                         const proton::Position& position,
                         const proton::SearchLimits& limits) {
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    return search->think(position, limits);
}

void test_repertoire_weights_and_randomness() {
    struct Fixture {
        std::vector<std::string> moves;
        std::vector<std::pair<std::string, int>> weights;
    };
    const Fixture fixtures[] = {
        {{}, {{"e2e4", 315}, {"d2d4", 215}, {"c2c4", 70}, {"g1f3", 55}}},
        {{"e2e4"}, {{"e7e5", 120}, {"c7c5", 110}, {"e7e6", 85}}},
        {{"d2d4"}, {{"d7d5", 110}, {"g8f6", 105}}},
    };
    for (const auto& fixture : fixtures) {
        auto position = position_after(fixture.moves);
        for (const int randomness : {0, 1, 70, 100}) {
            auto options = human_options();
            options.book_randomness = randomness;
            proton::Evaluator evaluator;
            evaluator.set_options(options);
            auto search = std::make_unique<proton::Search>(evaluator, options);
            Access::opportunity(*search, true);
            auto roots = Access::roots(*search, position);
            const auto before = roots;
            const auto random = Access::random(*search);
            const auto fen = position.fen();
            Access::apply(*search, position, roots);
            expect(random == Access::random(*search), "book priors consume no RNG draws");
            expect(position.fen() == fen, "book priors preserve position");
            double total_weight = 0.0;
            const double exponent = static_cast<double>(400 - 3 * randomness) / 100.0;
            for (const auto& [text, weight] : fixture.weights) {
                (void)text;
                total_weight += randomness == 0 ? (weight == fixture.weights.front().second ? 1.0 : 0.0)
                                                : std::pow(static_cast<double>(weight), exponent);
            }
            for (std::size_t index = 0; index < roots.size(); ++index) {
                const auto& root = roots[index];
                const auto& old = before[index];
                expect(root.move == old.move && root.score == old.score &&
                           root.previous_score == old.previous_score && root.exact == old.exact &&
                           root.pv == old.pv,
                       "book priors preserve root scores, bounds, PVs and ordering");
                expect(std::isfinite(root.book_prior) && root.book_prior >= 0.0F &&
                           root.book_prior <= std::log(16.0) + 0.00001,
                       "book influence has finite bounded odds");
                const auto entry = std::find_if(fixture.weights.begin(), fixture.weights.end(),
                    [&](const auto& item) { return item.first == root.move.to_uci(); });
                double probability = 0.0;
                if (entry != fixture.weights.end()) {
                    const int weight = entry->second;
                    probability = (randomness == 0 ? (weight == fixture.weights.front().second ? 1.0 : 0.0)
                                                  : std::pow(static_cast<double>(weight), exponent)) /
                                  total_weight;
                }
                expect(std::abs(std::expm1(root.book_prior) - 15.0 * probability) < 0.00002,
                       "prior follows the existing book power law: " + root.move.to_uci());
                expect(std::abs((root.policy - old.policy) * 0.20 - root.book_prior) < 0.00001,
                       "book ordering and sampling use the same bounded prior");
            }
        }
    }
}

void test_disabled_and_failed_opportunities() {
    const proton::Position position;
    proton::SearchLimits limits;
    limits.depth = 4;
    auto analysis = human_options();
    analysis.human_style = false;
    const auto reference = run(analysis, position, limits);
    auto no_book = analysis;
    no_book.use_book = false;
    expect(same_result(reference, run(no_book, position, limits)),
           "explicitly disabled human mode leaves fixed-depth analysis unchanged");

    for (const int variety : {0, 1}) {
        auto options = human_options();
        options.human_variety_percent = variety;
        // This seed's first opportunity draw is >= 1, so both cases skip selection.
        options.human_seed = 27;
        std::mt19937_64 random(options.human_seed);
        expect(random() % 100ULL >= static_cast<std::uint64_t>(variety),
               "failed-opportunity seed is valid");
        expect(same_result(reference, run(options, position, limits)),
               "zero/failed human opportunity retains the full analysis search");
    }

    auto options = human_options();
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    Access::opportunity(*search, true);
    options.use_book = false;
    search->set_options(options);  // The existing populated book must be ignored.
    auto roots = Access::roots(*search, position);
    const auto before = roots;
    Access::apply(*search, position, roots);
    for (std::size_t index = 0; index < roots.size(); ++index) {
        expect(roots[index].policy == before[index].policy && roots[index].book_prior == 0.0F,
               "UseBook false suppresses previously loaded priors");
    }
    expect(same_result(search->think(position, limits), run(options, position, limits)),
           "disabling an already loaded book matches a book-free human search");

    analysis.book_randomness = 0;
    limits.movetime_ms = 1000;
    const auto book_result = run(analysis, position, limits);
    expect(book_result.best.to_uci() == "e2e4" && book_result.nodes == 0 && book_result.depth == 0,
           "non-human timed book fast path keeps its previous highest-weight output");
}

void test_custom_legal_filter_and_limits() {
    auto options = human_options();
    struct TempBook {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("proton-human-book-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
        ~TempBook() { std::error_code error; std::filesystem::remove(path, error); }
    } file;
    {
        std::ofstream output(file.path);
        output << "2147483647 | a2a3\n2147483647 | e2e5\n";
    }
    options.book_file = file.path.string();
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    Access::opportunity(*search, true);
    proton::Position position;
    const auto a3 = position.parse_uci_move("a2a3");
    auto roots = Access::roots(*search, position);
    Access::apply(*search, position, roots);
    expect(root_for(roots, a3).book_prior > 2.7F &&
               root_for(roots, a3).book_prior <= std::log(16.0) + 0.00001,
           "legal custom book weights are accepted but remain bounded");

    // Bypass the file parser deliberately: a book entry is never itself proof
    // of legality, including its flags, promotion type, or square range.
    const std::vector<std::pair<proton::Move, int>> invalid = {
        {{12, 44, proton::NoPieceType, proton::MoveQuiet}, std::numeric_limits<int>::max()},
        {{255, 255, proton::Queen, proton::MovePromotion}, std::numeric_limits<int>::max()},
        {{12, 28, proton::Queen, proton::MovePromotion}, std::numeric_limits<int>::max()},
        {a3, 0},
    };
    Access::book(*search, position, invalid);
    roots = Access::roots(*search, position);
    Access::apply(*search, position, roots);
    expect(std::all_of(roots.begin(), roots.end(), [](const auto& root) {
        return root.book_prior == 0.0F;
    }), "malformed, illegal and nonpositive entries cannot create a prior");

    const auto nh3 = position.parse_uci_move("g1h3");
    Access::book(*search, position, {{a3, std::numeric_limits<int>::max()}});
    const std::vector<proton::Move> allowed = {nh3};
    roots = Access::roots(*search, position, &allowed);
    Access::apply(*search, position, roots);
    expect(roots.size() == 1 && roots.front().move == nh3 && roots.front().book_prior == 0.0F,
           "excluded book moves do not enter the filtered root pool");
    proton::SearchLimits limits;
    limits.depth = 3;
    limits.movetime_ms = 1000;
    limits.search_moves_specified = true;
    limits.search_moves = allowed;
    const auto restricted = search->think(position, limits);
    expect(restricted.best == nh3 && restricted.depth == limits.depth && restricted.nodes > 0,
           "human book play performs normal restricted search");
    limits.search_moves.clear();
    const auto empty = search->think(position, limits);
    expect(empty.best.is_null() && empty.nodes == 0,
           "book cannot escape an explicitly empty searchmoves restriction");

    limits.search_moves_specified = false;
    limits.movetime_ms = 0;
    for (const std::uint64_t cap : {1ULL, 2ULL, 16ULL, 64ULL}) {
        limits.node_limit = cap;
        const auto result = run(options, position, limits);
        const auto legal = Access::roots(*search, position);
        expect(result.nodes <= cap && std::any_of(legal.begin(), legal.end(), [&](const auto& root) {
            return root.move == result.best;
        }), "custom book honours tiny node budgets and returns a legal move");
    }
    limits.node_limit = 0;
    std::atomic<bool> stopped{true};
    limits.external_stops = {&stopped};
    const auto stopped_result = run(options, position, limits);
    expect(stopped_result.nodes == 0 && stopped_result.depth == 0,
           "custom priors respect external stop before search");
    limits.external_stops.clear();
    const auto deadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    limits.external_deadline = &deadline;
    const auto expired = run(options, position, limits);
    expect(expired.nodes == 0 && expired.depth == 0,
           "custom priors respect the inherited deadline");
}

void test_book_candidate_pool_and_confirmation() {
    auto options = human_options();
    options.human_skill = 20;
    options.book_randomness = 0;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::Position position;
    const auto target = position.parse_uci_move("e2e4");
    proton::SearchLimits restricted;
    restricted.depth = 3;
    restricted.search_moves_specified = true;
    restricted.search_moves = {target};
    auto analysis = options;
    analysis.use_book = false;
    analysis.human_style = false;
    const auto verified = run(analysis, position, restricted);
    auto roots = Access::roots(*search, position);
    const auto target_root = root_for(roots, target);
    std::erase_if(roots, [&](const auto& root) { return root.move == target; });
    roots.push_back(target_root);
    const auto incumbent = roots.front().move;
    for (auto& root : roots) {
        root.policy = 0.0F;
        root.score = verified.score_cp - 1000;
        root.exact = true;
    }
    roots.front().score = verified.score_cp;
    roots.back().score = verified.score_cp - 10000;  // A pessimistic PVS bound.
    roots.back().exact = false;
    Access::opportunity(*search, true);
    Access::apply(*search, position, roots);
    expect(roots.size() > 6 && roots.back().book_prior > 0.0F,
           "book fixture lies beyond the ordinary skill-20 candidate prefix");
    std::uint64_t selecting_seed = 0;
    for (std::uint64_t seed = 1; seed <= 64; ++seed) {
        auto sampled = roots;
        Access::prepare_selection(*search, seed);
        const auto selected = Access::select(*search, position, sampled, restricted.depth);
        if (selected == target) {
            selecting_seed = seed;
            const auto& confirmed = root_for(sampled, target);
            expect(Access::nodes(*search) > 0 && confirmed.exact &&
                       confirmed.score == verified.score_cp && confirmed.pv == verified.pv,
                   "out-of-prefix book alternative passes fresh restricted confirmation");
            break;
        }
    }
    expect(selecting_seed != 0, "bounded book prior admits a real confirmed alternative");
    if (selecting_seed == 0) return;
    for (const int excluded_score : {Access::no_score(), verified.score_cp - 1000}) {
        auto excluded = roots;
        excluded.back().score = excluded_score;
        excluded.back().exact = true;
        Access::prepare_selection(*search, selecting_seed);
        expect(Access::select(*search, position, excluded, restricted.depth) == incumbent &&
                   Access::nodes(*search) == 0,
               "book prior does not admit NoScore or exact out-of-band roots");
    }
    auto exhausted = roots;
    Access::prepare_selection(*search, selecting_seed, 100, 99);
    expect(Access::select(*search, position, exhausted, restricted.depth) == incumbent &&
               Access::nodes(*search) == 100,
           "book candidate falls back when confirmation exhausts its remaining node");
}

void test_poisoned_book_and_mate_protection() {
    auto options = human_options();
    options.human_skill = 20;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    auto position = position_after({"f2f3", "e7e5"});
    const auto poisoned = position.parse_uci_move("g2g4");
    const auto safe = position.parse_uci_move("e2e4");
    Access::book(*search, position, {{poisoned, std::numeric_limits<int>::max()}});
    Roots roots = {{safe, 0, 0, 0.0F, true, {safe}},
                   {poisoned, -1000, 0, 0.0F, false, {poisoned}}};
    Access::opportunity(*search, true);
    Access::apply(*search, position, roots);
    std::uint64_t rejecting_seed = 0;
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        auto sampled = roots;
        Access::prepare_selection(*search, seed);
        const auto selected = Access::select(*search, position, sampled, 3);
        expect(selected == safe, "a maximal book weight cannot select the Fool's Mate blunder");
        if (Access::nodes(*search) != 0) {
            rejecting_seed = seed;
            const auto& rejected = root_for(sampled, poisoned);
            expect(rejected.exact && rejected.score <= -proton::Search::mate_threshold(),
                   "poisoned book move was sampled and rejected by a searched mate score");
            break;
        }
    }
    expect(rejecting_seed != 0, "poisoned-book test actually exercises confirmation");
    if (rejecting_seed != 0) {
        auto sampled = roots;
        Access::prepare_selection(*search, rejecting_seed, 100, 99);
        expect(Access::select(*search, position, sampled, 3) == safe &&
                   Access::nodes(*search) == 100,
               "poisoned candidate cannot bypass an interrupted confirmation");
    }
    roots.front().score = proton::Search::mate_score() - 1;
    Access::prepare_selection(*search, 27);
    expect(Access::select(*search, position, roots, 3) == safe && Access::nodes(*search) == 0,
           "book priors cannot randomise away the searched mate incumbent");

    proton::SearchLimits limits;
    limits.depth = 3;
    limits.search_moves_specified = true;
    limits.search_moves = {safe, poisoned};
    const auto public_result = search->think(position, limits);
    expect(public_result.best == safe && public_result.depth == 3 && public_result.nodes > 0,
           "normal human root search rejects the custom poisoned book continuation");
}

void test_tiny_prior_retains_provisional_loss_penalty() {
    auto options = human_options();
    options.human_skill = 20;
    options.book_randomness = 100;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::Position position;
    const auto best = position.parse_uci_move("b1c3");
    const auto main_line = position.parse_uci_move("e2e4");
    const auto rare = position.parse_uci_move("d2d4");
    Access::book(*search, position, {{main_line, std::numeric_limits<int>::max()}, {rare, 1}});
    Roots roots = {{best, 1000, 0, 0.0F, true, {best}},
                   {main_line, 0, 0, 0.0F, true, {main_line}},
                   {rare, 0, 0, 0.0F, false, {rare}}};
    Access::opportunity(*search, true);
    Access::apply(*search, position, roots);
    expect(roots.back().book_prior > 0.0F && roots.back().book_prior < 0.00001F,
           "rare sideline fixture has only a tiny repertoire weight");
    // Choose a draw that would select the rare move if any positive book
    // membership erased its entire provisional loss penalty.
    std::uint64_t seed = 1;
    for (; seed <= 128; ++seed) {
        std::mt19937_64 random(seed);
        const double draw = static_cast<double>(random() >> 11U) / 9007199254740992.0;
        if (draw > 0.96 && draw < 0.98) break;
    }
    expect(seed <= 128, "rare-prior regression has a deterministic sampling draw");
    Access::prepare_selection(*search, seed);
    expect(Access::select(*search, position, roots, 3) == best && Access::nodes(*search) == 0,
           "a tiny positive book prior retains the pessimistic bound's sampling penalty");
}

void test_total_book_sampling_odds() {
    const proton::Position position;
    const auto best = position.parse_uci_move("e2e4");
    const auto alternative = position.parse_uci_move("d2d4");
    const auto excluded = position.parse_uci_move("g1f3");
    const auto logits = proton::human_move_logits(position, {best, alternative, excluded});
    const float maximum = *std::max_element(logits.begin(), logits.end());
    const auto human_weight = [&](std::size_t index) {
        return std::exp(std::clamp(static_cast<double>(logits[index] - maximum),
                                   -std::log(32.0), 0.0));
    };
    std::mt19937_64 random(1);
    const double draw = static_cast<double>(random() >> 11U) / 9007199254740992.0;
    constexpr double Margin = 0.0001;
    expect(draw > Margin && draw < 1.0 - Margin, "sampling boundary draw is interior");

    for (const int skill : {0, 8, 16, 20}) {
        auto options = human_options();
        options.human_skill = skill;
        options.book_randomness = 100;  // Book probabilities follow the raw weights.
        proton::Evaluator evaluator;
        evaluator.set_options(options);
        auto search = std::make_unique<proton::Search>(evaluator, options);
        const int gap = 20 - skill;
        const int allowance = std::min(700, 12 + 8 * gap + 2 * gap * gap);
        const double temperature = 4.0 + 3.5 * gap;
        if (skill == 16) expect(allowance == 76, "default skill-16 loss fixture is 76cp");
        for (const bool exact : {false, true}) {
            for (const int loss : {0, allowance / 2, allowance, allowance + 1000}) {
                if (exact && loss > allowance) continue;
                for (const int book_weight : {0, 1, 3}) {
                    // The excluded root contributes to book normalization but
                    // cannot enter sampling. The alternative's multipliers are
                    // independently known: 1, 1 + 15/4, and the maximum 16.
                    const int other_weight = book_weight == 3 ? 0 : 3;
                    const double book_factor = 1.0 + 15.0 * book_weight /
                                                         (book_weight + other_weight);
                    Access::book(*search, position,
                                 {{alternative, book_weight}, {excluded, other_weight}});
                    const double alternative_weight =
                        std::exp(-std::min(loss, allowance) / temperature) *
                        human_weight(1) / 1.18 * book_factor;
                    const double incumbent_factor = human_weight(0) * (1.0 + 0.55 * skill);
                    for (const bool sample_alternative : {false, true}) {
                        // Solve for an incumbent policy placing its predicted
                        // probability just above/below the fixed RNG draw. This
                        // probes the selector's combined weights, including
                        // both evaluation and policy, without exposing a helper.
                        const double probability = draw + (sample_alternative ? -Margin : Margin);
                        Roots roots = {{best, 1000, 0, static_cast<float>(std::log(
                            probability / (1.0 - probability) * alternative_weight /
                            incumbent_factor) / 0.20), true, {best}},
                            {alternative, 1000 - loss, 0, 0.0F, exact, {alternative}},
                            {excluded, -2000, 0, 0.0F, true, {excluded}}};
                        Access::opportunity(*search, true);
                        Access::apply(*search, position, roots);
                        // One node distinguishes sampling an alternative from
                        // the incumbent, while requiring fresh confirmation to
                        // fail closed even when its supplied bound was exact.
                        Access::prepare_selection(*search, 1, 1);
                        const auto selected = Access::select(*search, position, roots, 3);
                        const std::string context = "skill=" + std::to_string(skill) +
                            " exact=" + std::to_string(exact) + " loss=" + std::to_string(loss) +
                            " book=" + std::to_string(book_weight);
                        expect(selected == best && Access::nodes(*search) ==
                                   (sample_alternative ? 1ULL : 0ULL),
                               "total book odds match the independent probability boundary: " + context);
                    }
                }
            }
        }
    }
}

void test_seeded_opening_search() {
    for (const auto& moves : std::vector<std::vector<std::string>>{{}, {"e2e4"}, {"d2d4"}}) {
        const auto position = position_after(moves);
        auto options = human_options();
        proton::SearchLimits limits;
        limits.depth = 3;
        limits.movetime_ms = 1000;
        const auto first = run(options, position, limits);
        const auto second = run(options, position, limits);
        expect(first.depth == limits.depth && first.nodes > 0 && same_result(first, second),
               "human opening book searches reproducibly with a fixed seed");
        bool confirmed_alternative = false;
        // Correctly bounded book odds make alternatives rarer. This checks
        // that each opening can produce a verified alternative, not its rate.
        for (std::uint64_t seed = 1; seed <= 512 && !confirmed_alternative; ++seed) {
            options.human_seed = seed;
            proton::Evaluator evaluator;
            evaluator.set_options(options);
            auto search = std::make_unique<proton::Search>(evaluator, options);
            proton::Move main_best;
            const auto selected = search->think(position, limits, [&](const proton::SearchInfo& info) {
                if (info.depth == limits.depth && main_best.is_null() && !info.pv.empty()) {
                    main_best = info.pv.front();
                }
            });
            if (selected.best == main_best) continue;
            auto analysis = options;
            analysis.human_style = false;
            analysis.use_book = false;
            auto restricted = limits;
            restricted.movetime_ms = 0;
            restricted.search_moves_specified = true;
            restricted.search_moves = {selected.best};
            const auto reference = run(analysis, position, restricted);
            expect(selected.score_cp == reference.score_cp && selected.pv == reference.pv,
                   "returned book alternative reports its independently searched score and PV");
            confirmed_alternative = true;
        }
        expect(confirmed_alternative, "opening fixture exercises a confirmed human alternative");
    }
}

}  // namespace

int main() {
    test_repertoire_weights_and_randomness();
    test_disabled_and_failed_opportunities();
    test_custom_legal_filter_and_limits();
    test_book_candidate_pool_and_confirmation();
    test_poisoned_book_and_mate_protection();
    test_tiny_prior_retains_provisional_loss_penalty();
    test_total_book_sampling_odds();
    test_seeded_opening_search();
    if (failures != 0) {
        std::cerr << failures << " human-book test(s) failed\n";
        return 1;
    }
    std::cout << "human-book tests passed\n";
    return 0;
}
