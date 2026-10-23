#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "engine/eval.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

float policy_for(proton::Evaluator& evaluator, proton::Position& position,
                 const std::string& uci) {
    std::vector<proton::Move> legal;
    position.generate_legal_moves(legal);
    const proton::Move target = position.parse_uci_move(uci);
    expect(!target.is_null(), "policy move parses: " + uci);

    const std::string before = position.fen();
    const std::uint64_t key = position.key();
    const std::vector<float> scores = evaluator.policy_scores(position, legal);
    expect(scores.size() == legal.size(), "policy score count matches legal moves");
    expect(scores == evaluator.policy_scores(position, legal),
           "policy is deterministic: " + uci);
    expect(position.fen() == before && position.key() == key,
           "policy preserves the input position: " + uci);
    for (std::size_t index = 0; index < legal.size(); ++index) {
        if (legal[index] == target) return scores[index];
    }

    expect(false, "policy move is present in legal list: " + uci);
    return 0.0F;
}

// All fixtures have no castling or en-passant rights. Rank reflection preserves
// files and orthodox development squares while reversing the pawn direction.
float symmetric_policy(proton::Evaluator& evaluator, const std::string& fen,
                       const std::string& uci) {
    proton::Position position;
    expect(position.set_fen(fen), "policy FEN parses: " + fen);
    expect(position.castling_rights() == 0 &&
               position.en_passant_square() == proton::NoSquare,
           "symmetry fixture has no special rights");
    expect(!position.in_check(proton::opposite(position.side_to_move())),
           "fixture does not leave the previous mover in check: " + fen);

    std::string mirrored_fen;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const proton::Piece piece = position.piece_at((rank * 8 + file) ^ 56);
            if (piece == proton::Empty) {
                ++empty;
                continue;
            }
            if (empty != 0) mirrored_fen += static_cast<char>('0' + empty);
            empty = 0;
            mirrored_fen += proton::piece_to_char(proton::make_piece(
                proton::opposite(proton::piece_color(piece)),
                proton::piece_type(piece)));
        }
        if (empty != 0) mirrored_fen += static_cast<char>('0' + empty);
        if (rank != 0) mirrored_fen += '/';
    }
    mirrored_fen += position.side_to_move() == proton::White ? " b" : " w";
    mirrored_fen += " - - " + std::to_string(position.halfmove_clock()) + " " +
                    std::to_string(position.fullmove_number());
    proton::Position mirrored;
    expect(mirrored.set_fen(mirrored_fen), "colour-reversed FEN parses");
    std::string mirrored_uci = uci;
    mirrored_uci[1] = static_cast<char>('8' - (uci[1] - '1'));
    mirrored_uci[3] = static_cast<char>('8' - (uci[3] - '1'));

    const float score = policy_for(evaluator, position, uci);
    const float mirrored_score = policy_for(evaluator, mirrored, mirrored_uci);
    expect(std::abs(score - mirrored_score) < 0.001F,
           "white/black policy symmetry: " + uci);
    return score;
}

void test_phase_aware_king_policy() {
    proton::Evaluator evaluator;
    const std::string middlegame = "4k3/7q/8/8/8/8/Q7/4K3 w - - 0 1";
    const std::string endgame = "4k3/8/8/8/8/8/8/4K3 w - - 0 1";
    expect(symmetric_policy(evaluator, middlegame, "e1d2") <
               symmetric_policy(evaluator, middlegame, "e1f1"),
           "middlegame king does not seek the centre");
    expect(symmetric_policy(evaluator, endgame, "e1d2") >
               symmetric_policy(evaluator, endgame, "e1f1") + 0.4F,
           "endgame king seeks the centre even at fullmove one");
}

void test_finish_development_before_repeating_a_minor() {
    proton::Evaluator evaluator;
    const float repeat_early = symmetric_policy(
        evaluator, "r3k2q/8/8/8/8/5N2/8/1NBQKB1R w - - 0 4", "f3g5");
    const float repeat_late = symmetric_policy(
        evaluator, "r3k2q/8/8/8/8/5N2/8/1NBQKB1R w - - 0 20", "f3g5");
    const float repeat_after_development = symmetric_policy(
        evaluator, "r3k2q/8/8/8/8/5N2/8/3QK2R w - - 0 4", "f3g5");
    expect(repeat_after_development > repeat_early + 1.7F,
           "quiet repeat move is deprioritised while home minors remain");
    expect(std::abs(repeat_early - repeat_late) < 0.001F,
           "unfinished development does not expire at an arbitrary move number");

    const float ending_with_home_minors = symmetric_policy(
        evaluator, "4k3/8/8/8/8/5N2/8/1NB1KB2 w - - 0 1", "f3g5");
    const float ending = symmetric_policy(
        evaluator, "4k3/8/8/8/8/5N2/8/4K3 w - - 0 1", "f3g5");
    expect(std::abs(ending_with_home_minors - ending) < 0.001F,
           "home minors in reduced material do not imply an opening");

    const float wrong_home_squares = symmetric_policy(
        evaluator, "r3k2q/8/8/8/8/5N2/8/1BNQK2R w - - 0 4", "f3g5");
    expect(std::abs(wrong_home_squares - repeat_after_development) < 0.001F,
           "bishop on b1 and knight on c1 are not undeveloped home minors");
}

void test_tactical_minor_moves_are_not_penalised() {
    proton::Evaluator evaluator;
    struct Fixture {
        const char* prefix;
        const char* move;
        const char* context;
    };
    const Fixture fixtures[] = {
        {"r3k2q/8/8/6r1/8/5N2/8/", "f3g5", "capture"},
        {"r6q/8/4k3/8/8/5N2/8/", "f3g5", "quiet check"},
        {"4r2k/7q/8/8/8/5N2/8/", "f3e5", "check evasion"},
        {"r3k2q/8/8/8/4p3/5N2/8/", "f3g5", "safe retreat from attack"},
    };
    for (const Fixture& fixture : fixtures) {
        const std::string unfinished =
            std::string(fixture.prefix) + "1NBQKB1R w - - 0 4";
        const std::string finished =
            std::string(fixture.prefix) + "3QK2R w - - 0 4";
        proton::Position position;
        expect(position.set_fen(unfinished), "tactical context FEN parses");
        const proton::Move move = position.parse_uci_move(fixture.move);
        expect(!move.is_null(), "tactical context move is legal");
        if (std::string(fixture.context) == "capture") {
            expect(move.is_capture(), "capture fixture is a capture");
        } else {
            expect(!move.is_capture(), "quiet tactical fixture is not a capture");
        }
        if (std::string(fixture.context) == "quiet check") {
            expect(position.gives_check(move), "quiet-check fixture gives check");
        }
        if (std::string(fixture.context) == "check evasion") {
            expect(position.in_check(position.side_to_move()) &&
                       !position.is_square_attacked(move.from, proton::Black) &&
                       !position.gives_check(move),
                   "evasion is independently exercised without an attacked minor");
        }
        if (std::string(fixture.context) == "safe retreat from attack") {
            expect(position.is_square_attacked(move.from, proton::Black),
                   "retreat fixture starts with an attacked minor");
            proton::UndoState undo;
            expect(position.make_move(move, undo), "retreat is legal");
            expect(!position.is_square_attacked(move.to, proton::Black),
                   "retreat reaches a safe square");
        }
        expect(std::abs(symmetric_policy(evaluator, unfinished, fixture.move) -
                        symmetric_policy(evaluator, finished, fixture.move)) < 0.001F,
               std::string(fixture.context) + " is exempt from development penalty");
    }

    const float unsafe_repeat = symmetric_policy(
        evaluator, "r3k2q/8/8/2p5/4p3/5N2/8/1NBQKB1R w - - 0 4", "f3d4");
    const float unsafe_developed = symmetric_policy(
        evaluator, "r3k2q/8/8/2p5/4p3/5N2/8/3QK2R w - - 0 4", "f3d4");
    expect(unsafe_developed > unsafe_repeat + 1.7F,
           "moving from one attacked square to another is not a forced retreat");
}

void test_queen_development_context() {
    proton::Evaluator evaluator;
    const std::string unfinished = "r3k2q/8/8/8/8/5N2/8/1NBQKB1R w - - 0 4";
    const std::string finished = "r3k2q/8/8/8/8/5N2/8/3QK2R w - - 0 4";
    expect(symmetric_policy(evaluator, finished, "d1d2") >
               symmetric_policy(evaluator, unfinished, "d1d2") + 1.4F,
           "quiet queen activity waits for actual development");
    expect(std::abs(symmetric_policy(evaluator, unfinished, "d1d7") -
                    symmetric_policy(evaluator, finished, "d1d7")) < 0.001F,
           "checking queen move is not delayed for development");
}

void test_centralisation_improvement() {
    proton::Evaluator evaluator;
    const std::string fen = "7k/8/8/8/8/2N5/5N2/4K3 w - - 0 20";
    expect(symmetric_policy(evaluator, fen, "f2e4") >
               symmetric_policy(evaluator, fen, "c3e4") + 0.3F,
           "same destination favours improving the less central knight");
}

void test_rook_file_improvement() {
    proton::Evaluator evaluator;
    const std::string open = "4k3/8/8/8/8/8/P1P1K3/R7 w - - 0 20";
    const std::string semi_open = "4k3/5p2/8/8/8/8/P1P1K3/R7 w - - 0 20";
    const float open_file = symmetric_policy(evaluator, open, "a1f1");
    const float closed_file = symmetric_policy(evaluator, open, "a1c1");
    const float semi_open_file = symmetric_policy(evaluator, semi_open, "a1f1");
    expect(open_file > semi_open_file + 0.4F &&
               semi_open_file > closed_file + 0.6F,
           "rook prefers acquiring an open file, then a semi-open file");

    const float acquiring = symmetric_policy(
        evaluator, "4k3/8/8/8/8/8/2P1K3/2R5 w - - 0 20", "c1f1");
    const float shuffling = symmetric_policy(
        evaluator, "4k3/8/8/8/8/8/4K3/2R5 w - - 0 20", "c1f1");
    expect(acquiring > shuffling + 1.1F,
           "rook file bonus measures improvement rather than open-file shuffling");

    const float opening_by_capture = symmetric_policy(
        evaluator, "7k/8/8/p7/8/8/4K3/R7 w - - 0 20", "a1a5");
    const float still_semi_open = symmetric_policy(
        evaluator, "7k/p7/8/p7/8/8/4K3/R7 w - - 0 20", "a1a5");
    expect(opening_by_capture > still_semi_open + 0.4F,
           "rook capture can open a file by removing its last enemy pawn");
}

void test_endgame_passed_pawn_progress() {
    proton::Evaluator evaluator;
    const float passed = symmetric_policy(
        evaluator, "7k/8/8/P7/8/8/8/4K3 w - - 0 1", "a5a6");
    const float advanced = symmetric_policy(
        evaluator, "7k/8/P7/8/8/8/8/4K3 w - - 0 1", "a6a7");
    const float blocked = symmetric_policy(
        evaluator, "7k/p7/8/P7/8/8/8/4K3 w - - 0 1", "a5a6");
    const float adjacent_blocker = symmetric_policy(
        evaluator, "7k/1p6/8/P7/8/8/8/4K3 w - - 0 1", "a5a6");
    const float queen_position = symmetric_policy(
        evaluator, "3q3k/8/8/P7/8/8/8/3QK3 w - - 0 1", "a5a6");
    expect(passed > blocked + 1.0F && passed > adjacent_blocker + 1.0F,
           "endgame progress bonus requires a passed pawn across adjacent files");
    expect(advanced > passed + 0.2F && passed > 0.0F,
           "advanced endgame passer can progress away from the centre");
    expect(std::abs(queen_position - blocked) < 0.001F,
           "queen position does not receive the endgame pawn-progress bonus");

    const float doubled = symmetric_policy(
        evaluator, "7k/P7/8/P7/8/8/8/4K3 w - - 0 1", "a5a6");
    expect(std::abs(doubled - blocked) < 0.001F,
           "rear doubled pawn receives no progress bonus behind its own pawn");
}

}  // namespace

int main() {
    test_phase_aware_king_policy();
    test_finish_development_before_repeating_a_minor();
    test_tactical_minor_moves_are_not_penalised();
    test_queen_development_context();
    test_centralisation_improvement();
    test_rook_file_improvement();
    test_endgame_passed_pawn_progress();

    if (failures != 0) {
        std::cerr << failures << " human-policy test(s) failed\n";
        return 1;
    }
    std::cout << "human-policy tests passed\n";
    return 0;
}
