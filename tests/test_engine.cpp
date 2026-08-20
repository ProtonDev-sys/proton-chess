#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "engine/attacks.h"
#include "engine/eval.h"
#include "engine/search.h"
#include "engine/see.h"

namespace proton {

struct SearchTestAccess {
    enum class TestBound { Upper, Lower, Exact };

    struct EntrySnapshot {
        bool found = false;
        int score = 0;
        int static_eval = 0;
        int depth = 0;
        TestBound bound = TestBound::Upper;
        std::uint8_t generation = 0;
        Move move = Move::null();
    };

    static void set_generation(Search& search, std::uint8_t generation) {
        search.generation_ = generation;
    }

    static void store(Search& search, std::uint64_t key, int depth, int score,
                      int static_eval, TestBound bound, const Move& move) {
        Search::Bound internal_bound = Search::Bound::Upper;
        if (bound == TestBound::Lower) internal_bound = Search::Bound::Lower;
        if (bound == TestBound::Exact) internal_bound = Search::Bound::Exact;
        search.store(key, depth, score, static_eval, internal_bound, move, 0);
    }

    static EntrySnapshot probe(const Search& search, std::uint64_t key) {
        const Search::TTEntry* entry = search.probe(key);
        if (entry == nullptr) return {};

        TestBound bound = TestBound::Upper;
        if (entry->bound == Search::Bound::Lower) bound = TestBound::Lower;
        if (entry->bound == Search::Bound::Exact) bound = TestBound::Exact;
        return EntrySnapshot{
            true,
            Search::score_from_tt(entry->score, 0),
            entry->static_eval,
            entry->depth,
            bound,
            entry->generation,
            entry->move,
        };
    }

    static void set_confirmation_budget(Search& search, std::uint64_t limit,
                                        std::uint64_t already_used) {
        search.limits_ = SearchLimits{};
        search.limits_.node_limit = limit;
        search.nodes_ = already_used;
        search.stop_requested_.store(false, std::memory_order_relaxed);
    }

    static std::optional<SearchResult> confirm_candidate(
        Search& search, const Position& position, const Move& candidate, int depth) {
        return search.confirm_human_candidate(position, candidate, depth);
    }

    static int quiescence(Search& search, Position& position,
                          int alpha, int beta, int ply) {
        search.limits_ = SearchLimits{};
        search.stop_requested_.store(false, std::memory_order_relaxed);
        search.main_phase_ = false;
        search.main_budget_exhausted_ = false;
        search.has_hard_deadline_ = false;
        search.has_main_deadline_ = false;
        search.nodes_ = 0;
        search.selective_depth_ = 0;
        search.root_side_ = position.side_to_move();
        for (int& length : search.pv_length_) length = 0;
        return search.quiescence(position, alpha, beta, ply);
    }

    static std::vector<Move> pv(const Search& search, int ply) {
        return search.current_pv(ply);
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

void expect_perft(const std::string& fen, int depth, std::uint64_t expected,
                  const std::string& label) {
    proton::Position position;
    expect(position.set_fen(fen), label + " FEN parses");
    const std::uint64_t actual = position.perft(depth);
    expect(actual == expected, label + " perft(" + std::to_string(depth) + ") expected " +
                               std::to_string(expected) + ", got " + std::to_string(actual));
}

void test_make_unmake_and_hash() {
    proton::Position position;
    const std::string initial_fen = position.fen();
    const std::uint64_t initial_key = position.key();
    const std::uint64_t initial_pawn_key = position.pawn_key();
    std::vector<proton::Move> legal;
    position.generate_legal_moves(legal);
    for (const proton::Move& move : legal) {
        proton::UndoState undo;
        expect(position.make_move(move, undo), "start-position legal move makes");
        position.unmake_move(move, undo);
        expect(position.fen() == initial_fen, "make/unmake restores FEN for " + move.to_uci());
        expect(position.key() == initial_key, "make/unmake restores key for " + move.to_uci());
        expect(position.pawn_key() == initial_pawn_key,
               "make/unmake restores pawn key for " + move.to_uci());
    }
}

void test_random_make_unmake_and_hash() {
    std::mt19937 random(0x50524f54U);
    for (int game = 0; game < 10; ++game) {
        proton::Position position;
        const std::string initial_fen = position.fen();
        const std::uint64_t initial_key = position.key();
        const std::uint64_t initial_pawn_key = position.pawn_key();
        std::vector<std::pair<proton::Move, proton::UndoState>> played;

        for (int ply = 0; ply < 100; ++ply) {
            proton::Position reconstructed;
            expect(reconstructed.set_fen(position.fen()), "random position FEN reconstructs");
            expect(reconstructed.key() == position.key(), "incremental key matches reconstructed key");
            expect(reconstructed.pawn_key() == position.pawn_key(),
                   "incremental pawn key matches reconstructed pawn key");

            std::vector<proton::Move> legal;
            position.generate_legal_moves(legal);
            if (legal.empty()) break;
            const proton::Move move = legal[random() % legal.size()];
            proton::UndoState undo;
            expect(position.make_move(move, undo), "random legal move makes");
            played.emplace_back(move, undo);
        }

        while (!played.empty()) {
            position.unmake_move(played.back().first, played.back().second);
            played.pop_back();
        }
        expect(position.fen() == initial_fen, "random line unmake restores initial FEN");
        expect(position.key() == initial_key, "random line unmake restores initial key");
        expect(position.pawn_key() == initial_pawn_key,
               "random line unmake restores initial pawn key");
    }
}

void test_special_moves() {
    proton::Position position;

    expect(position.set_fen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"), "castle FEN parses");
    for (const std::string& text : {"e1g1", "e1c1"}) {
        const std::string before = position.fen();
        const std::uint64_t key = position.key();
        const proton::Move move = position.parse_uci_move(text);
        expect(!move.is_null(), text + " generated");
        proton::UndoState undo;
        expect(position.make_move(move, undo), text + " makes");
        position.unmake_move(move, undo);
        expect(position.fen() == before && position.key() == key, text + " restores state");
    }

    expect(position.set_fen("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1"), "en-passant FEN parses");
    const std::string ep_before = position.fen();
    const std::uint64_t ep_key = position.key();
    const proton::Move ep = position.parse_uci_move("e5d6");
    expect(!ep.is_null(), "en-passant generated");
    proton::UndoState ep_undo;
    expect(position.make_move(ep, ep_undo), "en-passant makes");
    position.unmake_move(ep, ep_undo);
    expect(position.fen() == ep_before && position.key() == ep_key, "en-passant restores state");

    expect(position.set_fen("4k3/P7/8/8/8/8/8/4K3 w - - 0 1"), "promotion FEN parses");
    std::vector<proton::Move> qmoves;
    position.generate_pseudo_captures(qmoves);
    int promotions = 0;
    for (const proton::Move& move : qmoves) promotions += move.is_promotion() ? 1 : 0;
    expect(promotions == 4, "quiescence generator includes four quiet promotions");

    expect(position.set_fen("4k3/8/8/8/8/8/8/4K3 w KQ - 0 1"),
           "castling-rights-without-rooks FEN parses");
    expect(position.parse_uci_move("e1g1").is_null() && position.parse_uci_move("e1c1").is_null(),
           "castling is not generated without the required rook");
}

void test_fen_and_ep_hashing() {
    proton::Position position;
    const std::string original_fen = position.fen();
    const std::uint64_t original_key = position.key();
    expect(!position.set_fen("4k3/8/8/8/8/8/4K3/4K3 w - - 0 1"),
           "FEN with two white kings is rejected");
    expect(position.fen() == original_fen && position.key() == original_key,
           "rejected FEN leaves the existing position unchanged");
    expect(!position.set_fen("P3k3/8/8/8/8/8/8/4K3 w - - 0 1"),
           "FEN with a pawn on the back rank is rejected");

    proton::Position no_ep;
    proton::Position irrelevant_ep;
    expect(no_ep.set_fen("4k3/8/8/8/4p3/8/8/4K3 w - - 0 1"), "no-EP FEN parses");
    expect(irrelevant_ep.set_fen("4k3/8/8/8/4p3/8/8/4K3 w - e6 0 1"),
           "irrelevant-EP FEN parses");
    expect(no_ep.key() == irrelevant_ep.key(),
           "irrelevant en-passant square does not change repetition key");

    proton::Position relevant_ep;
    proton::Position relevant_no_ep;
    expect(relevant_ep.set_fen("4k3/8/8/3Pp3/8/8/8/4K3 w - e6 0 1"),
           "relevant-EP FEN parses");
    expect(relevant_no_ep.set_fen("4k3/8/8/3Pp3/8/8/8/4K3 w - - 0 1"),
           "relevant no-EP FEN parses");
    expect(relevant_ep.key() != relevant_no_ep.key(),
           "capturable en-passant square changes repetition key");

    proton::Position pinned_ep;
    proton::Position pinned_no_ep;
    expect(pinned_ep.set_fen("4k3/8/8/r4pPK/8/8/8/8 w - f6 0 1"),
           "pinned en-passant FEN parses");
    expect(pinned_no_ep.set_fen("4k3/8/8/r4pPK/8/8/8/8 w - - 0 1"),
           "pinned no-EP FEN parses");
    expect(pinned_ep.parse_uci_move("g5f6").is_null(),
           "en-passant exposing the king is illegal");
    expect(pinned_ep.key() == pinned_no_ep.key(),
           "illegal en-passant right does not change the repetition key");
}

void test_repetition_and_null_move() {
    proton::Position position;
    const auto apply = [&](const std::string& text) {
        const proton::Move move = position.parse_uci_move(text);
        expect(!move.is_null(), "repetition move parses: " + text);
        proton::UndoState undo;
        expect(position.make_move(move, undo), "repetition move makes: " + text);
    };

    for (const std::string& move : {"g1f3", "g8f6", "f3g1", "f6g8"}) apply(move);
    expect(position.is_repetition(2), "one cycle is a twofold search repetition");
    expect(!position.is_repetition(3), "one cycle is not a threefold repetition");

    const std::string before_null = position.fen();
    const std::uint64_t key_before_null = position.key();
    const int clock_before_null = position.halfmove_clock();
    proton::UndoState null_undo;
    position.make_null_move(null_undo);
    expect(position.halfmove_clock() == clock_before_null,
           "null move does not advance the rule-50 clock");
    position.unmake_null_move(null_undo);
    expect(position.fen() == before_null && position.key() == key_before_null,
           "null make/unmake restores exact state");
    expect(position.is_repetition(2), "null search does not pollute real repetition history");

    for (const std::string& move : {"g1f3", "g8f6", "f3g1", "f6g8"}) apply(move);
    expect(position.is_repetition(3), "two cycles produce a threefold repetition");
}

void test_gives_check_prediction() {
    const std::vector<std::pair<std::string, std::string>> fixtures = {
        {"5bkb/7p/8/7Q/8/3B4/8/6K1 w - - 0 1", "checking captures"},
        {"8/8/8/R2pP2k/8/8/8/7K w - d6 0 1", "en-passant discovered check"},
        {"7k/8/8/8/r2Pp2K/8/8/8 b - d3 0 1", "black en-passant discovered check"},
        {"4k3/8/8/2p5/4N3/8/8/4R1K1 w - - 0 1", "rook discovered check"},
        {"6k1/8/1p6/3N4/8/8/B7/6K1 w - - 0 1", "bishop discovered check"},
        {"4k3/8/8/4p3/4K3/8/8/4R3 w - - 0 1", "destination reblocks rook"},
        {"8/7k/8/5Pp1/8/8/2B5/6K1 w - g6 0 1", "en-passant destination reblocks bishop"},
        {"k7/6P1/8/8/8/8/8/K7 w - - 0 1", "checking promotions"},
        {"5k2/8/8/8/8/8/8/4K2R w K - 0 1", "checking castle"},
        {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
         "kiwipete"},
    };

    const auto verify_position = [&](proton::Position& position,
                                     const std::string& label) {
        const std::string original_fen = position.fen();
        const std::uint64_t original_key = position.key();
        std::vector<proton::Move> pseudo;
        position.generate_pseudo_moves(pseudo);
        int legal_moves = 0;
        for (const proton::Move& move : pseudo) {
            const bool predicted = position.gives_check(move);
            proton::UndoState undo;
            if (!position.make_move(move, undo)) continue;
            ++legal_moves;
            const bool actual = position.in_check(position.side_to_move());
            position.unmake_move(move, undo);
            expect(predicted == actual,
                   label + " predicts checking status for " + move.to_uci());
        }
        expect(legal_moves != 0, label + " gives-check fixture has legal moves");
        expect(position.fen() == original_fen && position.key() == original_key,
               label + " gives-check scan preserves position state");
    };

    for (const auto& [fen, label] : fixtures) {
        proton::Position position;
        expect(position.set_fen(fen), label + " gives-check FEN parses");
        verify_position(position, label);
    }

    std::mt19937 random(0x43484543U);
    for (int game = 0; game < 12; ++game) {
        proton::Position position;
        for (int ply = 0; ply < 16; ++ply) {
            verify_position(position,
                            "random gives-check game " + std::to_string(game) +
                                " ply " + std::to_string(ply));
            std::vector<proton::Move> legal;
            position.generate_legal_moves(legal);
            if (legal.empty()) break;
            const proton::Move move = legal[random() % legal.size()];
            proton::UndoState undo;
            expect(position.make_move(move, undo),
                   "random gives-check playout move makes");
        }
    }
}

void test_draw_material() {
    proton::Position position;
    expect(position.set_fen("8/8/8/8/8/8/4k3/6K1 w - - 0 1"), "bare kings FEN");
    expect(position.is_draw_by_material(), "bare kings are dead material");

    expect(position.set_fen("8/8/8/8/8/8/4k1n1/6KN w - - 0 1"), "knight-v-knight FEN");
    expect(!position.is_draw_by_material(), "K+N versus K+N is not automatically dead material");

    expect(position.set_fen("8/8/8/8/8/8/4k1b1/6KN w - - 0 1"), "bishop-v-knight FEN");
    expect(!position.is_draw_by_material(), "K+B versus K+N is not automatically dead material");

    expect(position.set_fen("8/8/8/8/8/4k3/8/5NKN w - - 0 1"), "two-knights-v-king FEN");
    expect(!position.is_draw_by_material(),
           "K+NN versus K is not a FIDE dead position because a helpmate is legal");
}

void test_pawn_safe_minor_mobility() {
    proton::EngineOptions options;
    options.use_book = false;
    proton::Evaluator evaluator;
    evaluator.set_options(options);

    const auto evaluate = [&](const std::string& fen, const std::string& label) {
        proton::Position position;
        expect(position.set_fen(fen), label + " FEN parses");
        return evaluator.evaluate(position);
    };

    const int knight_unsafe = evaluate(
        "3k4/8/8/8/1pN5/8/8/3K4 w - - 0 1", "white unsafe knight mobility");
    const int knight_safe = evaluate(
        "3k4/8/8/8/2N3p1/8/8/3K4 w - - 0 1", "white safe knight mobility");
    const int knight_unsafe_mirror = evaluate(
        "3k4/8/8/1Pn5/8/8/8/3K4 b - - 0 1", "black unsafe knight mobility");
    const int knight_safe_mirror = evaluate(
        "3k4/8/8/2n3P1/8/8/8/3K4 b - - 0 1", "black safe knight mobility");
    expect(knight_safe - knight_unsafe == 4,
           "enemy pawn control removes one knight mobility square");
    expect(knight_unsafe == knight_unsafe_mirror &&
               knight_safe == knight_safe_mirror,
           "pawn-safe knight mobility is color symmetric");

    const int bishop_unsafe = evaluate(
        "3k4/1p6/8/8/2B5/8/8/3K4 w - - 0 1", "white unsafe bishop mobility");
    const int bishop_safe = evaluate(
        "3k4/6p1/8/8/2B5/8/8/3K4 w - - 0 1", "white safe bishop mobility");
    const int bishop_unsafe_mirror = evaluate(
        "3k4/8/8/2b5/8/8/1P6/3K4 b - - 0 1", "black unsafe bishop mobility");
    const int bishop_safe_mirror = evaluate(
        "3k4/8/8/2b5/8/8/6P1/3K4 b - - 0 1", "black safe bishop mobility");
    expect(bishop_safe - bishop_unsafe == 5,
           "enemy pawn control removes one bishop mobility square");
    expect(bishop_unsafe == bishop_unsafe_mirror &&
               bishop_safe == bishop_safe_mirror,
           "pawn-safe bishop mobility is color symmetric");

    const int rook_left_pawn = evaluate(
        "3k4/8/1p6/8/2R5/8/8/3K4 w - - 0 1", "rook left-pawn scope guard");
    const int rook_right_pawn = evaluate(
        "3k4/8/6p1/8/2R5/8/8/3K4 w - - 0 1", "rook right-pawn scope guard");
    expect(rook_left_pawn == rook_right_pawn,
           "pawn-safe mobility remains limited to knights and bishops");
}

void test_rook_behind_passed_pawn() {
    proton::EngineOptions options;
    options.use_book = false;
    proton::Evaluator evaluator;
    evaluator.set_options(options);

    const auto evaluate = [&](const std::string& fen, const std::string& label) {
        proton::Position position;
        expect(position.set_fen(fen), label + " FEN parses");
        return evaluator.evaluate(position);
    };

    const int white_support = evaluate(
        "7k/8/8/8/2p5/P1P5/8/R6K w - - 0 1", "white rook behind passer");
    const int white_control = evaluate(
        "7k/8/8/8/2p5/P1P5/8/2R4K w - - 0 1", "white rook control");
    const int black_support = evaluate(
        "k6r/8/5p1p/5P2/8/8/8/K7 b - - 0 1", "black rook behind passer");
    const int black_control = evaluate(
        "k4r2/8/5p1p/5P2/8/8/8/K7 b - - 0 1", "black rook control");
    expect(white_support == 741 && white_control == 724 &&
               white_support - white_control == 17,
           "an unobstructed own rook behind a passer earns the tapered bonus");
    expect(black_support == white_support && black_control == white_control,
           "rook-behind-passer scoring is color and side-to-move symmetric");

    const int white_front = evaluate(
        "7k/8/8/8/R1p5/P1P5/8/7K w - - 0 1", "white rook in front of passer");
    const int black_front = evaluate(
        "k7/8/5p1p/5P1r/8/8/8/K7 b - - 0 1", "black rook in front of passer");
    expect(white_front == 718 && black_front == white_front,
           "a rook in front of its passer does not earn the support bonus");

    const int white_blocked = evaluate(
        "7k/8/8/8/2p5/P1P5/p7/R6K w - - 0 1", "blocked white supporting rook");
    const int black_blocked = evaluate(
        "k6r/7P/5p1p/5P2/8/8/8/K7 b - - 0 1", "blocked black supporting rook");
    expect(white_blocked == 425 && black_blocked == white_blocked,
           "an intervening piece prevents the rook support bonus");

    const int white_enemy_support = evaluate(
        "7k/8/8/8/2p5/P1PP4/7K/r7 w - - 0 1", "enemy rook behind white passer");
    const int white_enemy_control = evaluate(
        "7k/8/8/8/2p5/P1PP4/7K/3r4 w - - 0 1", "enemy rook control");
    const int black_enemy_support = evaluate(
        "7R/k7/4pp1p/5P2/8/8/8/K7 b - - 0 1", "enemy rook behind black passer");
    const int black_enemy_control = evaluate(
        "4R3/k7/4pp1p/5P2/8/8/8/K7 b - - 0 1", "mirrored enemy rook control");
    expect(white_enemy_support == -326 &&
               white_enemy_support == white_enemy_control &&
               black_enemy_support == black_enemy_control &&
               black_enemy_support == white_enemy_support,
           "only an own rook behind the passer earns the support bonus");
}

proton::Bitboard slow_sliding_attacks(int square, proton::Bitboard occupied,
                                      const std::vector<int>& directions) {
    proton::Bitboard result = 0;
    for (const int delta : directions) {
        int current = square;
        while (true) {
            const int target = current + delta;
            if (!proton::attacks::valid_step(current, target, delta)) break;
            result |= proton::bit(target);
            if ((occupied & proton::bit(target)) != 0) break;
            current = target;
        }
    }
    return result;
}

void test_attack_tables() {
    std::mt19937_64 random(0x41545441434b5355ULL);
    const std::vector<int> bishop_directions = {9, 7, -9, -7};
    const std::vector<int> rook_directions = {8, -8, 1, -1};

    // Empty, full, single-blocker and random occupancies exercise every ray
    // direction and every edge square.
    std::vector<proton::Bitboard> occupancies = {0, ~proton::Bitboard{0}};
    for (int square = 0; square < 64; ++square) occupancies.push_back(proton::bit(square));
    for (int index = 0; index < 512; ++index) occupancies.push_back(random());

    for (const proton::Bitboard occupied : occupancies) {
        for (int square = 0; square < 64; ++square) {
            const proton::Bitboard expected_bishop =
                slow_sliding_attacks(square, occupied, bishop_directions);
            const proton::Bitboard expected_rook =
                slow_sliding_attacks(square, occupied, rook_directions);
            expect(proton::attacks::bishop(square, occupied) == expected_bishop,
                   "precomputed bishop rays match directional reference");
            expect(proton::attacks::rook(square, occupied) == expected_rook,
                   "precomputed rook rays match directional reference");
            expect(proton::attacks::queen(square, occupied) ==
                       (expected_bishop | expected_rook),
                   "queen attacks combine bishop and rook rays");
        }
    }
}

void test_static_exchange() {
    const auto evaluate = [](const std::string& fen, const std::string& text) {
        proton::Position position;
        expect(position.set_fen(fen), "SEE FEN parses for " + text);
        const proton::Move move = position.parse_uci_move(text);
        expect(!move.is_null(), "SEE move parses: " + text);
        return proton::static_exchange_eval(position, move);
    };

    expect(evaluate("6k1/8/8/3r4/8/8/3Q4/6K1 w - - 0 1", "d2d5") == 500,
           "SEE values an undefended rook capture");
    expect(evaluate("6k1/8/4p3/3p4/8/8/3Q4/6K1 w - - 0 1", "d2d5") == -850,
           "SEE rejects a queen capture recaptured by a pawn");
    expect(evaluate("3r2k1/8/8/3q4/8/8/3R4/6K1 w - - 0 1", "d2d5") == 450,
           "SEE follows a rook recapture");
    expect(evaluate("3r2k1/8/8/3q4/8/8/3R4/3Q2K1 w - - 0 1", "d2d5") == 950,
           "SEE follows x-ray recaptures and optimal stopping");
    expect(evaluate("6k1/P7/8/8/8/8/8/6K1 w - - 0 1", "a7a8q") == 850,
           "SEE includes quiet promotion gain");
    expect(evaluate("6k1/8/8/3pP3/8/8/8/6K1 w - d6 0 1", "e5d6") == 100,
           "SEE removes the en-passant victim from its real square");
    expect(evaluate("4k3/4n3/8/3p4/2P5/8/8/4R1K1 w - - 0 1", "c4d5") == 100,
           "SEE does not count a pinned recapturing knight");
    expect(evaluate("1n1k1r2/8/5P1p/2pppnp1/3NP3/8/r1QN2PP/2b1K1R1 b - - 0 32",
                    "c1d2") == -15,
           "SEE recognises a safe king recapture as the best exchange response");
    expect(evaluate("r2qk2r/ppp2p2/4bn1b/3pN1Qp/P6P/2Pn2P1/1P2PP2/RNBK1B1R b kq - 0 13",
                    "d3c1") == 15,
           "SEE terminates an exchange after a legal king capture");
    expect(evaluate("1N5k/P1K5/1r6/8/8/8/8/8 b - - 0 1", "b6b8") == -1030,
           "SEE prefers a safe promotion over a safe king recapture");
}

void test_search_tactics() {
    for (int elo = proton::UciEloMin; elo <= proton::UciEloLegacyTop; ++elo) {
        const proton::UciEloProfile profile = proton::uci_elo_profile(elo);
        const int legacy_loss = std::clamp(
            (proton::UciEloLegacyTop - elo) / 8, 8, 250);
        expect(profile.skill == std::clamp((elo - proton::UciEloMin) / 100, 0, 20) &&
                   profile.max_loss_cp == legacy_loss,
               "Elo profile preserves the legacy mapping at " + std::to_string(elo));
    }
    int previous_high_elo_loss = 8;
    for (int elo = proton::UciEloLegacyTop + 1; elo <= proton::UciEloMax; ++elo) {
        const proton::UciEloProfile profile = proton::uci_elo_profile(elo);
        expect(profile.skill == 20 && profile.max_loss_cp >= 0 &&
                   profile.max_loss_cp <= previous_high_elo_loss,
               "high-Elo profile is monotonic at " + std::to_string(elo));
        previous_high_elo_loss = profile.max_loss_cp;
    }
    const proton::UciEloProfile elo_800 = proton::uci_elo_profile(800);
    const proton::UciEloProfile elo_2800 = proton::uci_elo_profile(2800);
    const proton::UciEloProfile elo_2900 = proton::uci_elo_profile(2900);
    const proton::UciEloProfile elo_2999 = proton::uci_elo_profile(2999);
    const proton::UciEloProfile elo_3000 = proton::uci_elo_profile(3000);
    expect(elo_800.skill == 0 && elo_800.max_loss_cp == 250,
           "Elo 800 retains the legacy limiter profile");
    expect(elo_2800.skill == 20 && elo_2800.max_loss_cp == 8,
           "Elo 2800 retains the legacy limiter profile");
    expect(elo_2900.skill == 20 && elo_2900.max_loss_cp == 4,
           "Elo 2900 tapers the intentional loss allowance");
    expect(elo_2999.skill == 20 && elo_2999.max_loss_cp == 1,
           "only Elo 3000 reaches the zero-loss profile");
    expect(elo_3000.skill == 20 && elo_3000.max_loss_cp == 0,
           "Elo 3000 permits only equally scored alternatives");
    expect(proton::uci_elo_profile(proton::UciEloMin - 1).skill == elo_800.skill &&
               proton::uci_elo_profile(proton::UciEloMin - 1).max_loss_cp ==
                   elo_800.max_loss_cp,
           "Elo profile clamps below the advertised minimum");
    expect(proton::uci_elo_profile(proton::UciEloMax + 1).skill == elo_3000.skill &&
               proton::uci_elo_profile(proton::UciEloMax + 1).max_loss_cp ==
                   elo_3000.max_loss_cp,
           "Elo profile clamps above the advertised maximum");

    proton::EngineOptions options;
    options.use_book = false;
    options.hash_mb = 16;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    proton::Search search(evaluator);
    search.set_options(options);

    proton::Position mate;
    expect(mate.set_fen("7k/5Q2/6K1/8/8/8/8/8 w - - 99 1"), "mate-in-one FEN");
    proton::SearchLimits limits;
    limits.depth = 4;
    const proton::SearchResult result = search.think(mate, limits);
    expect(!result.best.is_null(), "search returns a move in mate-in-one position");
    proton::UndoState undo;
    expect(mate.make_move(result.best, undo), "mate move is legal");
    std::vector<proton::Move> replies;
    mate.generate_legal_moves(replies);
    expect(mate.in_check(mate.side_to_move()) && replies.empty(),
           "selected move checkmates (got " + result.best.to_uci() + ")");
    expect(result.score_cp >= proton::Search::mate_threshold(),
           "quiet mate reaching a 100-ply clock is scored as mate, not draw");

    options.human_style = true;
    options.human_skill = 0;
    options.human_max_loss_cp = 500;
    options.human_seed = 7;
    evaluator.set_options(options);
    search.set_options(options);
    search.new_game();
    proton::Position human_mate;
    expect(human_mate.set_fen("7k/5Q2/6K1/8/8/8/8/8 w - - 99 1"),
           "human-mode mate-in-one FEN");
    const proton::SearchResult human_result = search.think(human_mate, limits);
    proton::UndoState human_undo;
    expect(human_mate.make_move(human_result.best, human_undo),
           "human-mode mate move is legal");
    replies.clear();
    human_mate.generate_legal_moves(replies);
    expect(human_mate.in_check(human_mate.side_to_move()) && replies.empty(),
           "human mode never randomises away a forced mate");

    const std::string loss_band_fen =
        "3rk3/ppp2ppp/8/8/3Q4/8/PPP2PPP/4K3 w - - 0 1";
    proton::EngineOptions full_options;
    full_options.use_book = false;
    full_options.hash_mb = 1;
    full_options.uci_elo = 800;
    proton::Evaluator full_evaluator;
    full_evaluator.set_options(full_options);
    auto full_search = std::make_unique<proton::Search>(full_evaluator);
    full_search->set_options(full_options);
    proton::Position loss_band_position;
    expect(loss_band_position.set_fen(loss_band_fen), "human loss-band FEN parses");
    proton::SearchLimits loss_band_limits;
    loss_band_limits.depth = 4;
    const proton::SearchResult full_result =
        full_search->think(loss_band_position, loss_band_limits);

    const auto exact_score_for = [&](const proton::Move& selected, int depth = 4) {
        proton::EngineOptions verify_options;
        verify_options.use_book = false;
        verify_options.hash_mb = 1;
        proton::Evaluator verify_evaluator;
        verify_evaluator.set_options(verify_options);
        auto verify_search = std::make_unique<proton::Search>(verify_evaluator);
        verify_search->set_options(verify_options);
        proton::Position verify_position;
        expect(verify_position.set_fen(loss_band_fen), "loss-band verification FEN parses");
        proton::SearchLimits verify_limits;
        verify_limits.depth = depth;
        verify_limits.search_moves_specified = true;
        verify_limits.search_moves.push_back(selected);
        return verify_search->think(verify_position, verify_limits).score_cp;
    };

    bool selected_alternative = false;
    for (const std::uint64_t seed : std::vector<std::uint64_t>{1, 7, 27, 42, 99}) {
        proton::EngineOptions limited_options = full_options;
        limited_options.uci_limit_strength = true;
        limited_options.human_seed = seed;
        proton::Evaluator limited_evaluator;
        limited_evaluator.set_options(limited_options);
        auto limited_search = std::make_unique<proton::Search>(limited_evaluator);
        limited_search->set_options(limited_options);
        proton::Position limited_position;
        expect(limited_position.set_fen(loss_band_fen), "limited loss-band FEN parses");
        const proton::SearchResult limited_result =
            limited_search->think(limited_position, loss_band_limits);
        selected_alternative = selected_alternative || limited_result.best != full_result.best;
        const int verified_score = exact_score_for(limited_result.best);
        expect(full_result.score_cp - verified_score <= 700,
               "every human candidate stays inside its 700 cp loss band (seed " +
                   std::to_string(seed) + ", move " + limited_result.best.to_uci() + ")");
    }
    expect(selected_alternative, "fixed human seeds exercise a non-best in-band move");

    const std::string zero_loss_fen =
        "rnbqk1nr/ppp2p2/3pp1pp/8/2P1P3/2PPBN2/P4PPP/R2QKB1R b KQkq - 0 7";
    proton::SearchLimits zero_loss_limits;
    zero_loss_limits.depth = 3;
    proton::Evaluator zero_loss_full_evaluator;
    zero_loss_full_evaluator.set_options(full_options);
    auto zero_loss_full_search =
        std::make_unique<proton::Search>(zero_loss_full_evaluator, full_options);
    proton::Position zero_loss_full_position;
    expect(zero_loss_full_position.set_fen(zero_loss_fen), "zero-loss FEN parses");
    const proton::SearchResult zero_loss_full_result =
        zero_loss_full_search->think(zero_loss_full_position, zero_loss_limits);

    proton::EngineOptions elo_3000_options = full_options;
    elo_3000_options.uci_limit_strength = true;
    elo_3000_options.uci_elo = 3000;
    elo_3000_options.human_seed = 27;
    proton::Evaluator elo_3000_evaluator;
    elo_3000_evaluator.set_options(elo_3000_options);
    auto elo_3000_search =
        std::make_unique<proton::Search>(elo_3000_evaluator, elo_3000_options);
    proton::Position elo_3000_position;
    expect(elo_3000_position.set_fen(zero_loss_fen), "Elo 3000 FEN parses");
    const proton::SearchResult elo_3000_result =
        elo_3000_search->think(elo_3000_position, zero_loss_limits);
    expect(elo_3000_result.best != zero_loss_full_result.best,
           "Elo 3000 regression exercises a real alternative");

    proton::Evaluator zero_loss_verify_evaluator;
    zero_loss_verify_evaluator.set_options(full_options);
    auto zero_loss_verify_search =
        std::make_unique<proton::Search>(zero_loss_verify_evaluator, full_options);
    proton::Position zero_loss_verify_position;
    expect(zero_loss_verify_position.set_fen(zero_loss_fen),
           "zero-loss verification FEN parses");
    proton::SearchLimits zero_loss_verify_limits = zero_loss_limits;
    zero_loss_verify_limits.search_moves_specified = true;
    zero_loss_verify_limits.search_moves.push_back(elo_3000_result.best);
    const proton::SearchResult zero_loss_verify_result =
        zero_loss_verify_search->think(zero_loss_verify_position, zero_loss_verify_limits);
    expect(zero_loss_verify_result.score_cp >= zero_loss_full_result.score_cp,
           "Elo 3000 confirms an alternative is independently equal or better");

    proton::EngineOptions interrupted_options = full_options;
    interrupted_options.uci_limit_strength = true;
    interrupted_options.human_seed = 27;
    proton::Evaluator interrupted_evaluator;
    interrupted_evaluator.set_options(interrupted_options);
    auto interrupted_search = std::make_unique<proton::Search>(interrupted_evaluator);
    interrupted_search->set_options(interrupted_options);
    proton::Position interrupted_position;
    expect(interrupted_position.set_fen(loss_band_fen),
           "interrupted loss-band FEN parses");
    proton::SearchLimits interrupted_limits = loss_band_limits;
    interrupted_limits.node_limit = 3450;
    const proton::SearchResult interrupted_result =
        interrupted_search->think(interrupted_position, interrupted_limits);
    expect(interrupted_result.depth == 3,
           "reserved node budget keeps the last complete main-search depth");
    expect(interrupted_result.nodes > interrupted_limits.node_limit / 2 &&
               interrupted_result.nodes <= interrupted_limits.node_limit,
           "confirmation uses the reserve without exceeding the total node limit");
    expect(interrupted_result.best != full_result.best,
           "node-limited human play can return a real alternative");
    const int interrupted_best_score =
        exact_score_for(full_result.best, interrupted_result.depth);
    const int interrupted_selected_score =
        exact_score_for(interrupted_result.best, interrupted_result.depth);
    expect(interrupted_best_score - interrupted_selected_score <= 700,
           "node-limited confirmation keeps the selected move in band");

    proton::Evaluator deadline_evaluator;
    deadline_evaluator.set_options(interrupted_options);
    auto deadline_search =
        std::make_unique<proton::Search>(deadline_evaluator, interrupted_options);
    proton::Position deadline_position;
    expect(deadline_position.set_fen(loss_band_fen), "deadline FEN parses");
    auto caller_deadline = std::chrono::steady_clock::time_point::max();
    proton::SearchLimits deadline_limits = interrupted_limits;
    deadline_limits.external_deadline = &caller_deadline;
    const proton::SearchResult deadline_result = deadline_search->think(
        deadline_position,
        deadline_limits,
        [&](const proton::SearchInfo& info) {
            if (info.depth == 3) caller_deadline = std::chrono::steady_clock::now();
        });
    expect(caller_deadline != std::chrono::steady_clock::time_point::max(),
           "deadline regression reaches the reserved-budget transition");
    expect(deadline_result.depth == 3 && deadline_result.best == full_result.best &&
               deadline_result.nodes < deadline_limits.node_limit / 2,
           "caller deadline stays sticky and suppresses post-search randomisation");

    proton::Evaluator ponder_full_evaluator;
    ponder_full_evaluator.set_options(full_options);
    auto ponder_full_search =
        std::make_unique<proton::Search>(ponder_full_evaluator, full_options);
    proton::Position ponder_full_position;
    expect(ponder_full_position.set_fen(loss_band_fen), "ponder FEN parses");
    proton::SearchLimits ponder_limits;
    ponder_limits.depth = 3;
    ponder_limits.ponder = true;
    const proton::SearchResult ponder_full_result =
        ponder_full_search->think(ponder_full_position, ponder_limits);

    proton::Evaluator ponder_limited_evaluator;
    ponder_limited_evaluator.set_options(interrupted_options);
    auto ponder_limited_search =
        std::make_unique<proton::Search>(ponder_limited_evaluator, interrupted_options);
    proton::Position ponder_limited_position;
    expect(ponder_limited_position.set_fen(loss_band_fen),
           "limited ponder FEN parses");
    const proton::SearchResult ponder_limited_result =
        ponder_limited_search->think(ponder_limited_position, ponder_limits);
    expect(ponder_limited_result.best == ponder_full_result.best &&
               ponder_limited_result.nodes == ponder_full_result.nodes,
           "pre-hit bounded ponder returns the root best without confirmation");

    proton::Evaluator tiny_budget_evaluator;
    tiny_budget_evaluator.set_options(interrupted_options);
    auto tiny_budget_search =
        std::make_unique<proton::Search>(tiny_budget_evaluator, interrupted_options);
    proton::Position tiny_budget_position;
    expect(tiny_budget_position.set_fen(loss_band_fen), "tiny-budget FEN parses");
    std::vector<proton::Move> tiny_budget_legal;
    tiny_budget_position.generate_legal_moves(tiny_budget_legal);
    proton::SearchLimits tiny_budget_limits = loss_band_limits;
    tiny_budget_limits.node_limit = 2;
    const proton::SearchResult tiny_budget_result =
        tiny_budget_search->think(tiny_budget_position, tiny_budget_limits);
    expect(tiny_budget_result.nodes <= tiny_budget_limits.node_limit,
           "tiny human budget does not overshoot its node cap");
    expect(std::find(tiny_budget_legal.begin(), tiny_budget_legal.end(),
                     tiny_budget_result.best) != tiny_budget_legal.end(),
           "tiny human budget still returns a legal move");

    proton::Evaluator full_node_evaluator;
    full_node_evaluator.set_options(full_options);
    auto full_node_search =
        std::make_unique<proton::Search>(full_node_evaluator, full_options);
    proton::Position full_node_position;
    expect(full_node_position.set_fen(loss_band_fen), "full node-limit FEN parses");
    proton::SearchLimits full_node_limits;
    full_node_limits.node_limit = 6000;
    const proton::SearchResult full_node_result =
        full_node_search->think(full_node_position, full_node_limits);
    expect(full_node_result.best.to_uci() == "d4a7" && full_node_result.depth == 5 &&
               full_node_result.nodes == full_node_limits.node_limit,
           "full-strength search respects the exact node cap (move " +
               full_node_result.best.to_uci() + ", depth " +
               std::to_string(full_node_result.depth) + ", nodes " +
               std::to_string(full_node_result.nodes) + ")");

    proton::Evaluator elo_3000_node_evaluator;
    elo_3000_node_evaluator.set_options(elo_3000_options);
    auto elo_3000_node_search =
        std::make_unique<proton::Search>(elo_3000_node_evaluator, elo_3000_options);
    proton::Position elo_3000_node_position;
    expect(elo_3000_node_position.set_fen(loss_band_fen),
           "Elo 3000 node-limit FEN parses");
    const proton::SearchResult elo_3000_node_result =
        elo_3000_node_search->think(elo_3000_node_position, full_node_limits);
    expect(elo_3000_node_result.best == full_node_result.best &&
               elo_3000_node_result.depth == full_node_result.depth &&
               elo_3000_node_result.nodes == full_node_result.nodes,
           "zero-loss Elo 3000 keeps the full bounded search budget");

    const std::string confirmation_cancel_fen =
        "rnbqkbnr/pp1ppppp/8/8/2N5/3P4/PPP1PPPP/R1BQKBNR b KQkq - 0 3";
    proton::SearchLimits confirmation_depth;
    confirmation_depth.depth = 6;
    proton::Evaluator confirmation_full_evaluator;
    confirmation_full_evaluator.set_options(full_options);
    auto confirmation_full_search =
        std::make_unique<proton::Search>(confirmation_full_evaluator, full_options);
    proton::Position confirmation_full_position;
    expect(confirmation_full_position.set_fen(confirmation_cancel_fen),
           "confirmation-cancel FEN parses");
    const proton::SearchResult confirmation_full_result =
        confirmation_full_search->think(confirmation_full_position,
                                         confirmation_depth);
    expect(!confirmation_full_result.best.is_null() &&
               confirmation_full_result.depth == confirmation_depth.depth,
           "confirmation-cancel reference search completes (move " +
               confirmation_full_result.best.to_uci() + ", depth " +
               std::to_string(confirmation_full_result.depth) + ")");

    proton::EngineOptions confirmation_options = full_options;
    confirmation_options.uci_limit_strength = true;
    confirmation_options.uci_elo = 2700;

    const auto confirmation_score_for = [&](const proton::Move& selected) {
        proton::Evaluator verify_evaluator;
        verify_evaluator.set_options(full_options);
        auto verify_search =
            std::make_unique<proton::Search>(verify_evaluator, full_options);
        proton::Position verify_position;
        expect(verify_position.set_fen(confirmation_cancel_fen),
               "confirmation score-verification FEN parses");
        proton::SearchLimits verify_limits = confirmation_depth;
        verify_limits.search_moves_specified = true;
        verify_limits.search_moves.push_back(selected);
        return verify_search->think(verify_position, verify_limits).score_cp;
    };

    const proton::Move confirmation_alternative =
        confirmation_full_position.parse_uci_move("g8f6");
    expect(!confirmation_alternative.is_null() &&
               confirmation_alternative != confirmation_full_result.best,
           "confirmation alternative parses and differs from the root best");
    const int confirmation_reference_score =
        confirmation_score_for(confirmation_full_result.best);
    const int confirmation_alternative_score =
        confirmation_score_for(confirmation_alternative);
    const int confirmation_loss =
        confirmation_reference_score - confirmation_alternative_score;
    constexpr int confirmation_allowance = 22;
    expect(confirmation_loss >= 0 &&
               confirmation_loss <= confirmation_allowance,
           "the confirmation alternative is independently inside the 22 cp "
           "allowance (loss " + std::to_string(confirmation_loss) + ")");

    proton::SearchLimits confirmation_wide_limits = confirmation_depth;
    confirmation_wide_limits.node_limit = 22000;
    confirmation_wide_limits.search_moves_specified = true;
    confirmation_wide_limits.search_moves = {
        confirmation_full_result.best, confirmation_alternative};
    proton::SearchResult confirmation_wide_result =
        confirmation_full_result;
    std::uint64_t confirmation_seed = 0;
    for (std::uint64_t seed = 1; seed <= 128; ++seed) {
        proton::EngineOptions seeded_options = confirmation_options;
        seeded_options.human_seed = seed;
        proton::Evaluator seeded_evaluator;
        seeded_evaluator.set_options(seeded_options);
        auto seeded_search = std::make_unique<proton::Search>(
            seeded_evaluator, seeded_options);
        proton::Position seeded_position;
        expect(seeded_position.set_fen(confirmation_cancel_fen),
               "seeded confirmation FEN parses");
        const proton::SearchResult seeded_result =
            seeded_search->think(seeded_position, confirmation_wide_limits);
        if (seeded_result.depth == confirmation_depth.depth &&
            seeded_result.best == confirmation_alternative) {
            confirmation_seed = seed;
            confirmation_wide_result = seeded_result;
            break;
        }
    }
    expect(confirmation_seed != 0,
           "a deterministic seed admits the independently in-band "
           "confirmation alternative");
    if (confirmation_seed != 0) {
        expect(confirmation_wide_result.nodes >
                   confirmation_wide_limits.node_limit / 2 &&
                   confirmation_wide_result.nodes <=
                       confirmation_wide_limits.node_limit,
               "wide confirmation uses its reserve without exceeding the "
               "parent node budget (seed " +
                   std::to_string(confirmation_seed) + ", nodes " +
                   std::to_string(confirmation_wide_result.nodes) + ")");
    }

    confirmation_options.human_seed = 37;
    proton::Evaluator confirmation_tight_evaluator;
    confirmation_tight_evaluator.set_options(confirmation_options);
    auto confirmation_tight_search =
        std::make_unique<proton::Search>(confirmation_tight_evaluator,
                                         confirmation_options);
    proton::Position confirmation_tight_position;
    expect(confirmation_tight_position.set_fen(confirmation_cancel_fen),
           "tight confirmation-cancel FEN parses");
    proton::SearchLimits confirmation_tight_limits = confirmation_depth;
    confirmation_tight_limits.node_limit = 11000;
    const proton::SearchResult confirmation_tight_result =
        confirmation_tight_search->think(confirmation_tight_position,
                                          confirmation_tight_limits);
    expect(confirmation_tight_result.depth == 6 &&
               confirmation_tight_result.best == confirmation_full_result.best,
           "exhausted confirmation falls back to the completed root best");
    expect(confirmation_tight_result.nodes >
               confirmation_tight_limits.node_limit / 2 &&
               confirmation_tight_result.nodes <= confirmation_tight_limits.node_limit,
           "cancelled confirmation stays inside the parent's total node budget (nodes " +
               std::to_string(confirmation_tight_result.nodes) + ", limit " +
               std::to_string(confirmation_tight_limits.node_limit) + ")");

    std::atomic<bool> primary_stop{false};
    std::atomic<bool> secondary_stop{true};
    proton::Evaluator dual_stop_evaluator;
    dual_stop_evaluator.set_options(full_options);
    proton::Search dual_stop_search(dual_stop_evaluator, full_options);
    proton::Position dual_stop_position;
    expect(dual_stop_position.set_fen(loss_band_fen), "dual-stop FEN parses");
    proton::SearchLimits dual_stop_limits = loss_band_limits;
    dual_stop_limits.external_stops = {&primary_stop, &secondary_stop};
    const proton::SearchResult dual_stop_result =
        dual_stop_search.think(dual_stop_position, dual_stop_limits);
    expect(dual_stop_result.depth == 0 && dual_stop_result.nodes == 0,
           "either external cancellation source stops the search");

    const std::string tight_band_fen =
        "1rbqk2r/pp2ppbp/2p2np1/3p4/3P4/P1NBP3/1PP2PPP/R2QK1NR b KQk - 2 7";
    proton::EngineOptions tight_full_options;
    tight_full_options.use_book = false;
    tight_full_options.hash_mb = 1;
    proton::Evaluator tight_full_evaluator;
    tight_full_evaluator.set_options(tight_full_options);
    auto tight_full_search = std::make_unique<proton::Search>(tight_full_evaluator);
    tight_full_search->set_options(tight_full_options);
    proton::Position tight_full_position;
    expect(tight_full_position.set_fen(tight_band_fen), "tight loss-band FEN parses");
    proton::SearchLimits tight_limits;
    tight_limits.depth = 5;
    const proton::SearchResult tight_full_result =
        tight_full_search->think(tight_full_position, tight_limits);

    const auto tight_score_for = [&](const proton::Move& selected) {
        proton::Evaluator verify_evaluator;
        verify_evaluator.set_options(tight_full_options);
        auto verify_search = std::make_unique<proton::Search>(
            verify_evaluator, tight_full_options);
        proton::Position verify_position;
        expect(verify_position.set_fen(tight_band_fen),
               "tight score-verification FEN parses");
        proton::SearchLimits verify_limits = tight_limits;
        verify_limits.search_moves_specified = true;
        verify_limits.search_moves.push_back(selected);
        return verify_search->think(verify_position, verify_limits).score_cp;
    };

    const proton::Move tight_best = tight_full_result.best;
    const proton::Move tight_alternative =
        tight_full_position.parse_uci_move("b7b6");
    expect(!tight_best.is_null() && !tight_alternative.is_null() &&
               tight_best != tight_alternative,
           "tight-band best and alternative are distinct legal moves");
    const int tight_best_score = tight_score_for(tight_best);
    const int tight_alternative_score = tight_score_for(tight_alternative);
    const int tight_alternative_loss =
        tight_best_score - tight_alternative_score;
    expect(tight_alternative_loss > 22 &&
               tight_alternative_loss <= 50,
           "the tight-band alternative is independently outside 22 cp but "
           "inside 50 cp (best " + tight_best.to_uci() + ", alternative " +
               tight_alternative.to_uci() + ", loss " +
               std::to_string(tight_alternative_loss) + ")");

    proton::SearchLimits tight_restricted_limits = tight_limits;
    tight_restricted_limits.search_moves_specified = true;
    tight_restricted_limits.search_moves = {
        tight_best, tight_alternative};

    proton::EngineOptions tight_wide_options = tight_full_options;
    tight_wide_options.human_skill = 19;
    // Skill 19 adds 8 + 2 cp, giving a 50 cp effective allowance.
    tight_wide_options.human_max_loss_cp = 40;
    proton::SearchResult tight_wide_result = tight_full_result;
    std::uint64_t tight_seed = 0;
    for (std::uint64_t seed = 1; seed <= 128; ++seed) {
        proton::EngineOptions seeded_options = tight_wide_options;
        seeded_options.human_seed = seed;
        proton::Evaluator seeded_evaluator;
        seeded_evaluator.set_options(seeded_options);
        auto seeded_search = std::make_unique<proton::Search>(
            seeded_evaluator, seeded_options);
        proton::Position seeded_position;
        expect(seeded_position.set_fen(tight_band_fen),
               "seeded tight-band FEN parses");
        const proton::SearchResult seeded_result =
            seeded_search->think(seeded_position,
                                 tight_restricted_limits);
        if (seeded_result.best == tight_alternative) {
            tight_seed = seed;
            tight_wide_result = seeded_result;
            break;
        }
    }
    expect(tight_seed != 0 &&
               tight_wide_result.best == tight_alternative,
           "a deterministic seed admits the alternative inside the 50 cp "
           "allowance");

    if (tight_seed != 0) {
        proton::EngineOptions tight_low_options = tight_wide_options;
        // Effective allowance: 12 + 8 + 2 = 22 cp.
        tight_low_options.human_max_loss_cp = 12;
        tight_low_options.human_seed = tight_seed;
        proton::Evaluator tight_low_evaluator;
        tight_low_evaluator.set_options(tight_low_options);
        auto tight_low_search = std::make_unique<proton::Search>(
            tight_low_evaluator, tight_low_options);
        proton::Position tight_low_position;
        expect(tight_low_position.set_fen(tight_band_fen),
               "low-allowance tight-band FEN parses");
        const proton::SearchResult tight_low_result =
            tight_low_search->think(tight_low_position,
                                    tight_restricted_limits);
        expect(tight_low_result.best == tight_best,
               "the same seed rejects the alternative outside the 22 cp "
               "allowance (seed " + std::to_string(tight_seed) + ")");
    }

    proton::EngineOptions book_options;
    book_options.use_book = true;
    book_options.hash_mb = 1;
    proton::Evaluator book_evaluator;
    book_evaluator.set_options(book_options);
    auto book_search = std::make_unique<proton::Search>(book_evaluator);
    book_search->set_options(book_options);
    proton::SearchLimits book_limits;
    book_limits.depth = 2;
    book_limits.movetime_ms = 1000;
    const proton::SearchResult book_result =
        book_search->think(proton::Position{}, book_limits);
    expect(book_result.depth == 0 && book_result.nodes == 0,
           "full-strength timed play may use the opening book");
    book_options.uci_limit_strength = true;
    book_options.human_seed = 27;
    book_evaluator.set_options(book_options);
    book_search->set_options(book_options);
    book_search->new_game();
    const proton::SearchResult limited_book_result =
        book_search->think(proton::Position{}, book_limits);
    expect(limited_book_result.depth == 2 && limited_book_result.nodes > 0,
           "limited play searches instead of bypassing the limiter through the book");

    options.human_style = false;
    options.human_skill = 20;
    options.human_max_loss_cp = 12;
    options.human_seed = 0;
    evaluator.set_options(options);
    search.set_options(options);
    search.new_game();

    proton::Position start;
    proton::SearchLimits restricted;
    restricted.depth = 3;
    restricted.search_moves_specified = true;
    restricted.search_moves.push_back(start.parse_uci_move("e2e4"));
    const proton::SearchResult restricted_result = search.think(start, restricted);
    expect(restricted_result.best.to_uci() == "e2e4", "searchmoves restriction is honoured");

    proton::Position stalemate;
    expect(stalemate.set_fen("k7/2Q5/2K5/8/8/8/8/8 w - - 0 1"),
           "stalemate-horizon FEN");
    proton::SearchLimits stalemate_limits;
    stalemate_limits.depth = 1;
    stalemate_limits.search_moves_specified = true;
    stalemate_limits.search_moves.push_back(stalemate.parse_uci_move("c7b6"));
    const proton::SearchResult stalemate_result = search.think(stalemate, stalemate_limits);
    expect(stalemate_result.best.to_uci() == "c7b6",
           "restricted stalemating move is searched");
    expect(stalemate_result.score_cp == 0,
           "quiescence scores stalemate as a draw, not static material");

    search.new_game();
    proton::Position quiet_mate_horizon;
    expect(quiet_mate_horizon.set_fen(
               "3r2k1/ppB2pp1/6b1/7p/1n1Pq3/2Q3P1/PP3P1P/2KR3R w - - 1 22"),
           "quiet-mate horizon FEN");
    proton::SearchLimits quiet_mate_limits;
    quiet_mate_limits.depth = 1;
    quiet_mate_limits.search_moves_specified = true;
    quiet_mate_limits.search_moves.push_back(quiet_mate_horizon.parse_uci_move("c3b4"));
    const proton::SearchResult quiet_mate_result =
        search.think(quiet_mate_horizon, quiet_mate_limits);
    expect(quiet_mate_result.best.to_uci() == "c3b4",
           "restricted poisoned queen move is searched");
    expect(quiet_mate_result.score_cp <= -proton::Search::mate_threshold(),
           "quiescence sees the opponent's immediate quiet mate");
    expect(quiet_mate_result.pv.size() >= 2 && quiet_mate_result.pv[1].to_uci() == "e4c2",
           "quiet mate is preserved in the principal variation");
}

void test_tt_same_key_replacement() {
    proton::EngineOptions options;
    options.hash_mb = 1;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    proton::Search search(evaluator, options);

    using Access = proton::SearchTestAccess;
    using Bound = Access::TestBound;
    constexpr std::uint64_t key = 0x0123456789abcdefULL;
    const proton::Move move_a{12, 28, proton::NoPieceType, proton::MoveQuiet};
    const proton::Move move_b{11, 27, proton::NoPieceType, proton::MoveQuiet};

    Access::set_generation(search, 7);
    Access::store(search, key, 10, 123, 45, Bound::Lower, move_a);
    Access::store(search, key, 2, -70, -25, Bound::Upper, move_b);
    Access::EntrySnapshot entry = Access::probe(search, key);
    expect(entry.found && entry.depth == 10 && entry.score == 123 &&
               entry.static_eval == 45 && entry.bound == Bound::Lower,
           "shallow same-key bound preserves deeper TT value data");
    expect(entry.move == move_b,
           "shallow same-key write can refresh the TT move independently");

    Access::store(search, key, 7, -40, -15, Bound::Upper, proton::Move::null());
    entry = Access::probe(search, key);
    expect(entry.depth == 10 && entry.score == 123 && entry.static_eval == 45 &&
               entry.bound == Bound::Lower,
           "shallow null same-key bound also preserves deeper TT value data");
    expect(entry.move == move_b,
           "rejected shallow null write preserves the existing TT move");

    Access::store(search, key, 8, 81, 19, Bound::Upper, proton::Move::null());
    entry = Access::probe(search, key);
    expect(entry.depth == 8 && entry.score == 81 && entry.static_eval == 19 &&
               entry.bound == Bound::Upper,
           "near-depth same-key bound replaces older TT value data");
    expect(entry.move == move_b, "null same-key write preserves the existing TT move");

    Access::store(search, key, 1, 7, 3, Bound::Exact, proton::Move::null());
    entry = Access::probe(search, key);
    expect(entry.depth == 1 && entry.score == 7 && entry.static_eval == 3 &&
               entry.bound == Bound::Exact,
           "shallow exact same-key result replaces deeper bound data");
    expect(entry.move == move_b, "exact null write does not erase the TT move");

    constexpr std::uint64_t aged_key = 0xfedcba9876543210ULL;
    Access::set_generation(search, 11);
    Access::store(search, aged_key, 12, 210, 80, Bound::Lower, move_a);
    Access::set_generation(search, 12);
    Access::store(search, aged_key, 2, -12, -6, Bound::Upper, move_b);
    entry = Access::probe(search, aged_key);
    expect(entry.depth == 12 && entry.score == 210 && entry.static_eval == 80 &&
               entry.bound == Bound::Lower && entry.generation == 12,
           "retained same-key result is refreshed into the current generation");
    expect(entry.move == move_b, "aged same-key write still refreshes its move");

    // All keys share the same low bits and therefore the same one-megabyte
    // table bucket. The fifth write evicts the shallowest entry.
    constexpr std::uint64_t bucket_index = 0x1234ULL;
    for (int slot = 0; slot < 4; ++slot) {
        const std::uint64_t collision = bucket_index |
            (static_cast<std::uint64_t>(slot + 1) << 32U);
        Access::store(search, collision, slot + 1, 20 + slot, 10 + slot,
                      Bound::Lower, move_a);
    }
    constexpr std::uint64_t replacement_key = bucket_index | (9ULL << 32U);
    Access::store(search, replacement_key, 6, 90, 30, Bound::Lower,
                  proton::Move::null());
    entry = Access::probe(search, replacement_key);
    expect(entry.found && entry.move.is_null(),
           "different-key replacement clears an unrelated TT move");
}

void test_confirmation_node_cap() {
    proton::EngineOptions options;
    options.use_book = false;
    options.hash_mb = 1;
    proton::Evaluator evaluator;
    evaluator.set_options(options);
    auto search = std::make_unique<proton::Search>(evaluator, options);
    proton::Position position;
    const proton::Move candidate = position.parse_uci_move("c2c4");
    expect(!candidate.is_null(), "confirmation node-cap candidate parses");

    proton::Evaluator child_evaluator;
    child_evaluator.set_options(options);
    auto child_search =
        std::make_unique<proton::Search>(child_evaluator, options);
    proton::SearchLimits child_limits;
    child_limits.depth = 8;
    child_limits.node_limit = 4000;
    child_limits.search_moves_specified = true;
    child_limits.search_moves.push_back(candidate);
    const proton::SearchResult child_result =
        child_search->think(position, child_limits);
    expect(child_result.nodes == child_limits.node_limit && child_result.depth < 8,
           "restricted child itself stops exactly at its node cap");

    constexpr std::uint64_t TotalNodes = 4100;
    constexpr std::uint64_t AlreadyUsed = 100;
    proton::SearchTestAccess::set_confirmation_budget(
        *search, TotalNodes, AlreadyUsed);
    const std::optional<proton::SearchResult> result =
        proton::SearchTestAccess::confirm_candidate(
            *search, position, candidate, 8);
    expect(!result.has_value(),
           "node-capped confirmation does not report an incomplete search");
    expect(proton::SearchTestAccess::nodes(*search) == TotalNodes,
           "child confirmation exhausts work inside the parent node cap (nodes " +
               std::to_string(proton::SearchTestAccess::nodes(*search)) + ")");

    auto no_room_search =
        std::make_unique<proton::Search>(evaluator, options);
    proton::SearchTestAccess::set_confirmation_budget(
        *no_room_search, AlreadyUsed, AlreadyUsed);
    const std::optional<proton::SearchResult> no_room_result =
        proton::SearchTestAccess::confirm_candidate(
            *no_room_search, position, candidate, 8);
    expect(!no_room_result.has_value() &&
               proton::SearchTestAccess::nodes(*no_room_search) == AlreadyUsed,
           "confirmation declines when the parent has no remaining node budget");
}

void test_qsearch_keeps_delta_pruned_checking_captures() {
    proton::EngineOptions options;
    options.use_book = false;
    options.hash_mb = 1;
    options.contempt_cp = 0;

    proton::Evaluator evaluator;
    evaluator.set_options(options);
    proton::Search search(evaluator, options);

    proton::Position position;
    expect(position.set_fen(
               "5bkb/7p/8/7Q/8/3B4/8/6K1 w - - 0 1"),
           "checking-capture delta-pruning FEN parses");
    constexpr int Ply = 4;
    const int stand_pat = evaluator.evaluate(position);
    const int alpha = stand_pat + proton::piece_value(proton::Pawn) + 180;
    const std::uint64_t original_key = position.key();
    const int score = proton::SearchTestAccess::quiescence(
        search, position, alpha, alpha + 1, Ply);
    expect(score == proton::Search::mate_score() - Ply - 1,
           "qsearch preserves a delta-prunable checking mate (got " +
               std::to_string(score) + ")");

    const std::vector<proton::Move> pv =
        proton::SearchTestAccess::pv(search, Ply);
    expect(pv.size() == 1 &&
               (pv.front().to_uci() == "h5h7" ||
                pv.front().to_uci() == "d3h7"),
           "qsearch PV contains the checking mate");
    expect(proton::SearchTestAccess::nodes(search) >= 2,
           "checking-mate qsearch enters the preserved tactical line (got " +
               std::to_string(proton::SearchTestAccess::nodes(search)) + ")");
    expect(position.key() == original_key,
           "pre-move check prediction preserves position state");

    proton::Search ep_search(evaluator, options);
    proton::Position ep_position;
    expect(ep_position.set_fen(
               "8/8/8/R2pP2k/8/8/8/7K w - d6 0 1"),
           "qsearch en-passant discovered-check FEN parses");
    const std::string ep_fen = ep_position.fen();
    const std::uint64_t ep_key = ep_position.key();
    const int ep_stand_pat = evaluator.evaluate(ep_position);
    const int ep_alpha =
        ep_stand_pat + proton::piece_value(proton::Pawn) + 180;
    static_cast<void>(proton::SearchTestAccess::quiescence(
        ep_search, ep_position, ep_alpha, ep_alpha + 1, Ply));
    expect(proton::SearchTestAccess::nodes(ep_search) >= 2,
           "qsearch enters a delta-prunable en-passant discovered check");
    expect(ep_position.fen() == ep_fen && ep_position.key() == ep_key,
           "en-passant checking-capture qsearch preserves position state");
}

}  // namespace

int main() {
    expect_perft("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
                 4, 197281, "start position");
    expect_perft("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
                 3, 97862, "kiwipete");
    expect_perft("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
                 4, 43238, "perft position 3");
    expect_perft("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1",
                 3, 9467, "perft position 4");
    expect_perft("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8",
                 3, 62379, "perft position 5");
    expect_perft("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10",
                 3, 89890, "perft position 6");
    test_make_unmake_and_hash();
    test_random_make_unmake_and_hash();
    test_special_moves();
    test_fen_and_ep_hashing();
    test_repetition_and_null_move();
    test_gives_check_prediction();
    test_draw_material();
    test_pawn_safe_minor_mobility();
    test_rook_behind_passed_pawn();
    test_attack_tables();
    test_static_exchange();
    test_search_tactics();
    test_tt_same_key_replacement();
    test_confirmation_node_cap();
    test_qsearch_keeps_delta_pruned_checking_captures();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all engine tests passed\n";
    return EXIT_SUCCESS;
}
