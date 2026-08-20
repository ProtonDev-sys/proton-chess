#include <cmath>
#include <iostream>
#include <string>

#include "engine/eval.h"

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

int white_score(proton::Evaluator& evaluator, const std::string& fen,
                const std::string& label) {
    proton::Position position;
    if (!position.set_fen(fen)) {
        expect(false, label + " FEN parses");
        return 0;
    }
    const int score = evaluator.evaluate(position);
    return position.side_to_move() == proton::White ? score : -score;
}

int side_score(proton::Evaluator& evaluator, const std::string& fen,
               const std::string& label) {
    proton::Position position;
    if (!position.set_fen(fen)) {
        expect(false, label + " FEN parses");
        return 0;
    }
    return evaluator.evaluate(position);
}

void test_executable_threat_is_stronger() {
    proton::Evaluator evaluator;

    const int attacked_white = white_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 w - - 0 1",
        "attacked queen with White to move");
    const int attacked_black = white_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 b - - 0 1",
        "attacked queen with Black to move");
    const int safe_white = white_score(
        evaluator, "8/8/7k/5p2/3Q4/8/8/K7 w - - 0 1",
        "safe queen with White to move");
    const int safe_black = white_score(
        evaluator, "8/8/7k/5p2/3Q4/8/8/K7 b - - 0 1",
        "safe queen with Black to move");

    const int attacked_gap = attacked_white - attacked_black;
    const int safe_gap = safe_white - safe_black;
    expect(attacked_gap >= safe_gap + 24,
           "a pawn attack that can be executed immediately has a larger "
           "side-to-move swing than the safe control (attacked=" +
               std::to_string(attacked_gap) + ", safe=" +
               std::to_string(safe_gap) + ")");
}

void test_defence_reduces_threat_penalty() {
    proton::Evaluator evaluator;

    const int loose_white = white_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 w - - 0 1",
        "loose queen with White to move");
    const int loose_black = white_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 b - - 0 1",
        "loose queen with Black to move");
    const int defended_white = white_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 w - - 0 1",
        "defended queen with White to move");
    const int defended_black = white_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 b - - 0 1",
        "defended queen with Black to move");

    const int loose_gap = loose_white - loose_black;
    const int defended_gap = defended_white - defended_black;
    expect(loose_gap >= defended_gap + 12,
           "an undefended queen has a larger executable-threat penalty than "
           "the same queen defended by a knight (loose=" +
               std::to_string(loose_gap) + ", defended=" +
               std::to_string(defended_gap) + ")");
}

void test_threat_term_is_colour_symmetric() {
    proton::Evaluator evaluator;
    const int white = side_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 w - - 0 1",
        "white threatened queen");
    const int black = side_score(
        evaluator, "7k/8/8/4q3/5P2/K7/8/8 b - - 0 1",
        "mirrored black threatened queen");

    expect(std::abs(white - black) <= 1,
           "threat evaluation is invariant under colour swap and 180-degree "
           "rotation (white=" + std::to_string(white) + ", black=" +
               std::to_string(black) + ")");
}

}  // namespace

int main() {
    test_executable_threat_is_stronger();
    test_defence_reduces_threat_penalty();
    test_threat_term_is_colour_symmetric();

    if (failures != 0) {
        std::cerr << failures << " threat-evaluation test(s) failed\n";
        return 1;
    }
    std::cout << "Threat-evaluation tests passed\n";
    return 0;
}
