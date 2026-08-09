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

    const auto exact_score_for = [&](const proton::Move& selected) {
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
        verify_limits.depth = 4;
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
    expect(interrupted_result.nodes >= interrupted_limits.node_limit,
           "loss-band verification reaches the cancellation node limit");
    expect(full_result.score_cp - exact_score_for(interrupted_result.best) <= 700,
           "an interrupted verification never admits an out-of-band move");

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

    proton::EngineOptions tight_limited_options = tight_full_options;
    tight_limited_options.uci_limit_strength = true;
    tight_limited_options.uci_elo = 2700;
    tight_limited_options.human_seed = 27;
    proton::Evaluator tight_limited_evaluator;
    tight_limited_evaluator.set_options(tight_limited_options);
    auto tight_limited_search =
        std::make_unique<proton::Search>(tight_limited_evaluator);
    tight_limited_search->set_options(tight_limited_options);
    proton::Position tight_limited_position;
    expect(tight_limited_position.set_fen(tight_band_fen),
           "limited tight loss-band FEN parses");
    const proton::SearchResult tight_limited_result =
        tight_limited_search->think(tight_limited_position, tight_limits);
    expect(tight_limited_result.best == tight_full_result.best,
           "fresh confirmation rejects the unsafe tight-band alternative");

    proton::Evaluator tight_verify_evaluator;
    tight_verify_evaluator.set_options(tight_full_options);
    auto tight_verify_search =
        std::make_unique<proton::Search>(tight_verify_evaluator);
    tight_verify_search->set_options(tight_full_options);
    proton::Position tight_verify_position;
    expect(tight_verify_position.set_fen(tight_band_fen),
           "tight loss-band verification FEN parses");
    proton::SearchLimits tight_verify_limits = tight_limits;
    tight_verify_limits.search_moves_specified = true;
    tight_verify_limits.search_moves.push_back(tight_limited_result.best);
    const proton::SearchResult tight_verify_result =
        tight_verify_search->think(tight_verify_position, tight_verify_limits);
    expect(tight_full_result.score_cp - tight_verify_result.score_cp <= 22,
           "Elo 2700 alternative stays inside its 22 cp loss band");

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
    test_draw_material();
    test_attack_tables();
    test_static_exchange();
    test_search_tactics();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all engine tests passed\n";
    return EXIT_SUCCESS;
}
