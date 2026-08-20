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

void test_defended_piece_pressure() {
    proton::Evaluator evaluator;

    const int loose_attacked = white_score(
        evaluator, "8/8/7k/2p5/3Q4/8/8/K7 w - - 0 1",
        "loose attacked queen");
    const int loose_safe = white_score(
        evaluator, "8/8/7k/5p2/3Q4/8/8/K7 w - - 0 1",
        "loose safe queen");
    const int defended_attacked = white_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 w - - 0 1",
        "defended attacked queen");
    const int defended_safe = white_score(
        evaluator, "8/8/7k/5p2/3Q4/5N2/8/K7 w - - 0 1",
        "defended safe queen");

    const int loose_gap = loose_safe - loose_attacked;
    const int defended_gap = defended_safe - defended_attacked;
    expect(defended_gap >= loose_gap + 8,
           "a defended queen attacked by a pawn carries a bounded pressure "
           "penalty beyond the matching loose-piece control (defended=" +
               std::to_string(defended_gap) + ", loose=" +
               std::to_string(loose_gap) + ")");
}

void test_pressure_is_side_to_move_independent() {
    proton::Evaluator evaluator;

    const int attacked_white = white_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 w - - 0 1",
        "attacked queen with White to move");
    const int safe_white = white_score(
        evaluator, "8/8/7k/5p2/3Q4/5N2/8/K7 w - - 0 1",
        "safe queen with White to move");
    const int attacked_black = white_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 b - - 0 1",
        "attacked queen with Black to move");
    const int safe_black = white_score(
        evaluator, "8/8/7k/5p2/3Q4/5N2/8/K7 b - - 0 1",
        "safe queen with Black to move");

    const int white_gap = safe_white - attacked_white;
    const int black_gap = safe_black - attacked_black;
    expect(std::abs(white_gap - black_gap) <= 1,
           "static pressure is independent of whose turn it is (white=" +
               std::to_string(white_gap) + ", black=" +
               std::to_string(black_gap) + ")");
}

void test_pressure_is_colour_symmetric() {
    proton::Evaluator evaluator;
    const int white = side_score(
        evaluator, "8/8/7k/2p5/3Q4/5N2/8/K7 w - - 0 1",
        "white defended threatened queen");
    const int black = side_score(
        evaluator, "7k/8/2n5/4q3/5P2/K7/8/8 b - - 0 1",
        "mirrored black defended threatened queen");

    expect(std::abs(white - black) <= 1,
           "pressure evaluation is invariant under colour swap and "
           "180-degree rotation (white=" + std::to_string(white) +
               ", black=" + std::to_string(black) + ")");
}

}  // namespace

int main() {
    test_defended_piece_pressure();
    test_pressure_is_side_to_move_independent();
    test_pressure_is_colour_symmetric();

    if (failures != 0) {
        std::cerr << failures << " threat-evaluation test(s) failed\n";
        return 1;
    }
    std::cout << "Threat-evaluation tests passed\n";
    return 0;
}
