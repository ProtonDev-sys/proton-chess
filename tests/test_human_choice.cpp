#include <chrono>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "engine/search.h"

namespace proton {

struct SearchTestAccess {
    static Move select_pair(Search& search, const Position& position,
                            const Move& incumbent, int best_score,
                            const Move& candidate, int depth, std::uint64_t seed,
                            bool force_candidate, std::uint64_t node_limit = 0) {
        search.limits_ = {};
        search.limits_.node_limit = node_limit;
        search.nodes_ = 0;
        search.stop_requested_.store(false);
        search.main_phase_ = false;
        search.main_budget_exhausted_ = false;
        search.has_main_deadline_ = false;
        search.has_hard_deadline_ = false;
        search.selection_opportunity_ = true;
        search.selection_budget_reserved_ = node_limit != 0;
        search.start_time_ = std::chrono::steady_clock::now();
        search.random_.seed(seed);
        std::vector<Search::RootMove> roots = {
            {incumbent, best_score, best_score, 0.0F, true, {incumbent}},
            {candidate, best_score - 1, best_score - 1,
             force_candidate ? 1000.0F : 0.0F, false, {candidate}},
        };
        return search.select_human_move(position, roots, depth);
    }

    static std::optional<SearchResult> shallow_reference(
        Search& search, const Position& position, const SearchLimits& limits) {
        search.limits_ = limits;
        search.nodes_ = 0;
        search.stop_requested_.store(false);
        search.main_phase_ = false;
        search.main_budget_exhausted_ = false;
        search.has_main_deadline_ = false;
        search.has_hard_deadline_ = false;
        search.start_time_ = std::chrono::steady_clock::now();
        return search.verify_human_search(position, 2);
    }

    static std::uint64_t nodes(const Search& search) { return search.nodes_; }
};

}  // namespace proton

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

struct Evaluation {
    proton::SearchResult result;
    int shallow = -40000;
};

Evaluation evaluate(const proton::Position& position, const proton::Move& move, int depth) {
    proton::EngineOptions options;
    options.hash_mb = 1;
    options.use_book = false;
    options.human_style = false;
    proton::Evaluator evaluator;
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::SearchLimits limits;
    limits.depth = depth;
    limits.search_moves_specified = !move.is_null();
    if (!move.is_null()) limits.search_moves = {move};
    Evaluation result;
    result.result = search->think(position, limits, [&](const proton::SearchInfo& info) {
        if (info.depth == 2) result.shallow = info.score_cp;
    });
    expect(result.result.depth == depth, "fixture completes its search");
    return result;
}

void test_plausible_mistakes() {
    proton::Position position;
    expect(position.set_fen("rnbq1rk1/ppp2ppp/5n2/4p3/1bB5/1PN1P1P1/P1QP1P1P/R1B1K1NR b KQ - 0 7"),
           "plausible bishop development fixture");
    const auto best = position.parse_uci_move("b8c6");
    const auto tempting = position.parse_uci_move("c8g4");
    const auto reference = evaluate(position, best, 6);
    const auto alternative = evaluate(position, tempting, 6);
    expect(reference.result.score_cp - alternative.result.score_cp > 25 &&
               reference.result.score_cp - alternative.result.score_cp <= 76,
           "tempting development loses a meaningful amount inside the club allowance");
    expect(alternative.shallow >= reference.shallow - 35,
           "the development mistake looks plausible after a short look");
    auto options = proton::club_play_options();
    options.hash_mb = 1;
    options.use_book = false;
    proton::Evaluator evaluator;
    auto search = std::make_unique<proton::Search>(evaluator, options);
    expect(proton::SearchTestAccess::select_pair(*search, position, best,
               reference.result.score_cp, tempting, 6, 1, true) == tempting,
           "fresh confirmation admits a plausible deeper mistake");

    expect(position.set_fen("6k1/5ppp/8/8/4n3/3P1N2/5PPP/6K1 w - - 0 20"),
           "obvious missed material fixture");
    const auto capture = position.parse_uci_move("d3e4");
    const auto quiet = position.parse_uci_move("h2h3");
    const auto capture_eval = evaluate(position, capture, 4);
    const auto quiet_eval = evaluate(position, quiet, 4);
    const int loss = capture_eval.result.score_cp - quiet_eval.result.score_cp;
    expect(loss > 25 && loss <= 700 && quiet_eval.shallow < capture_eval.shallow - 35,
           "quiet move is already clearly wrong at shallow depth despite the loose loss band");
    options.human_skill = 0;
    options.human_max_loss_cp = 500;
    search->set_options(options);
    expect(proton::SearchTestAccess::select_pair(*search, position, capture,
               capture_eval.result.score_cp, quiet, 4, 1, true) == capture,
           "selector rejects an obvious material miss even when its deep loss is allowed");
}

proton::Position repeated_knights(bool missing_pawn) {
    proton::Position position;
    if (missing_pawn) {
        expect(position.set_fen("rnbqkbnr/1ppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"),
               "drawing repetition defence fixture");
    }
    for (const char* text : {"g1f3", "g8f6", "f3g1", "f6g8", "g1f3", "g8f6", "f3g1"}) {
        proton::UndoState undo;
        expect(position.make_move(position.parse_uci_move(text), undo), "repetition setup move");
    }
    return position;
}

void test_repetition_preferences() {
    auto options = proton::club_play_options();
    options.hash_mb = 1;
    options.use_book = false;
    proton::Evaluator evaluator;
    auto search = std::make_unique<proton::Search>(evaluator, options);
    auto position = repeated_knights(false);
    auto repeating = position.parse_uci_move("f6g8");
    auto developing = position.parse_uci_move("b8c6");
    auto repeated = position;
    proton::UndoState undo;
    expect(repeated.make_move(repeating, undo) && repeated.is_repetition(3),
           "incumbent actually claims a threefold repetition");
    const auto continuation = evaluate(position, developing, 4);
    expect(continuation.result.score_cp >= 0, "development preserves at least the drawing score");
    int continuations = 0;
    for (std::uint64_t seed = 1; seed <= 32; ++seed) {
        if (proton::SearchTestAccess::select_pair(*search, position, repeating, 0,
                developing, 4, seed, false) == developing) ++continuations;
    }
    expect(continuations > 16, "safe continuation is preferred over cycling across fixed seeds");

    position = repeated_knights(true);
    repeating = position.parse_uci_move("f6g8");
    developing = position.parse_uci_move("b8c6");
    const auto losing = evaluate(position, developing, 4);
    expect(losing.result.score_cp < 0 && losing.result.score_cp >= -700,
           "declining the defensive repetition concedes an allowed but negative score");
    options.human_skill = 0;
    options.human_max_loss_cp = 500;
    search->set_options(options);
    expect(proton::SearchTestAccess::select_pair(*search, position, repeating, 0,
               developing, 4, 1, true) == repeating,
           "anti-repetition preference preserves a draw when the alternative loses");
}

void test_shallow_reference_ignores_retained_deeper_tt() {
    proton::Position position;
    expect(position.set_fen("r3kbnr/pp3ppp/2n1b3/2pqp3/8/2P2N1P/PP1PQPP1/RNB1KB1R w KQkq - 2 7"),
           "retained-TT shallow reference fixture");
    const auto cold = evaluate(position, proton::Move::null(), 2);
    const auto candidate = position.parse_uci_move("f3g5");
    const auto alternative = evaluate(position, candidate, 7);
    proton::EngineOptions options;
    options.hash_mb = 1;
    options.use_book = false;
    options.human_style = false;
    proton::Evaluator evaluator;
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::SearchLimits limits;
    limits.depth = 7;
    expect(search->think(position, limits).depth == 7, "fixture warms deeper TT entries");
    limits.depth = 2;
    const auto warm = search->think(position, limits);
    const int loss = warm.score_cp - alternative.result.score_cp;
    expect(warm.best != candidate && loss > 25 && loss <= 76,
           "candidate is a meaningful in-band alternative to the warm incumbent");
    expect(warm.score_cp - alternative.shallow > 35 &&
               alternative.shallow >= cold.shallow - 35,
           "warm TT score would reject a candidate plausible against fresh shallow analysis");
    options = proton::club_play_options();
    options.hash_mb = 1;
    options.use_book = false;
    search->set_options(options);
    expect(proton::SearchTestAccess::select_pair(*search, position, warm.best,
               warm.score_cp, candidate, 7, 1, true) == candidate,
           "shallow plausibility uses a fresh reference despite retained deeper TT entries");
    const auto budget = alternative.result.nodes + 1;
    expect(proton::SearchTestAccess::select_pair(*search, position, warm.best,
               warm.score_cp, candidate, 7, 1, true, budget) == warm.best &&
               proton::SearchTestAccess::nodes(*search) == budget,
           "insufficient shallow-reference reserve falls back inside the total node limit");
}

void test_shallow_reference_limits() {
    auto options = proton::club_play_options();
    options.hash_mb = 1;
    options.use_book = false;
    proton::Evaluator evaluator;
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::Position position;
    const auto only_move = position.parse_uci_move("g1f3");
    proton::SearchLimits limits;
    limits.search_moves_specified = true;
    limits.search_moves = {only_move};
    auto reference = proton::SearchTestAccess::shallow_reference(*search, position, limits);
    expect(reference.has_value() && reference->best == only_move &&
               reference->score_cp == evaluate(position, only_move, 2).result.score_cp,
           "fresh shallow reference respects the parent's searchmoves");
    limits.node_limit = 1;
    reference = proton::SearchTestAccess::shallow_reference(*search, position, limits);
    expect(!reference.has_value() && proton::SearchTestAccess::nodes(*search) == 1,
           "shallow reference cannot bypass a one-node budget");
    limits.node_limit = 0;
    std::atomic<bool> stopped{true};
    limits.external_stops = {&stopped};
    expect(!proton::SearchTestAccess::shallow_reference(*search, position, limits).has_value() &&
               proton::SearchTestAccess::nodes(*search) == 0,
           "shallow reference respects external cancellation");
    limits.external_stops.clear();
    const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    limits.external_deadline = &expired;
    expect(!proton::SearchTestAccess::shallow_reference(*search, position, limits).has_value() &&
               proton::SearchTestAccess::nodes(*search) == 0,
           "shallow reference respects an expired external deadline");
}

}  // namespace

int main() {
    test_plausible_mistakes();
    test_repetition_preferences();
    test_shallow_reference_ignores_retained_deeper_tt();
    test_shallow_reference_limits();
    if (failures != 0) return EXIT_FAILURE;
    std::cout << "human choice tests passed\n";
    return EXIT_SUCCESS;
}
