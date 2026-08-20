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

    const std::vector<float> scores = evaluator.policy_scores(position, legal);
    expect(scores.size() == legal.size(), "policy score count matches legal moves");
    for (std::size_t index = 0; index < legal.size(); ++index) {
        if (legal[index] == target) return scores[index];
    }

    expect(false, "policy move is present in legal list: " + uci);
    return 0.0F;
}

void test_phase_aware_king_policy() {
    proton::Evaluator evaluator;
    proton::Position middlegame;
    proton::Position endgame;

    expect(middlegame.set_fen(
               "4k3/7q/8/8/8/8/Q7/4K3 w - - 0 1"),
           "queen middlegame FEN parses");
    expect(endgame.set_fen(
               "4k3/8/8/8/8/8/8/4K3 w - - 0 1"),
           "bare-king endgame FEN parses");

    const float middlegame_centralisation =
        policy_for(evaluator, middlegame, "e1d2");
    const float endgame_centralisation =
        policy_for(evaluator, endgame, "e1d2");

    expect(endgame_centralisation > middlegame_centralisation + 2.5F,
           "king centralisation is encouraged only after major material is gone");
}

void test_finish_development_before_repeating_a_minor() {
    proton::Evaluator evaluator;
    proton::Position undeveloped;
    proton::Position developed;

    expect(undeveloped.set_fen(
               "4k3/8/8/8/8/5N2/8/1NB1KB2 w - - 0 1"),
           "undeveloped-minor FEN parses");
    expect(developed.set_fen(
               "4k3/8/8/8/8/5N2/8/4K3 w - - 0 1"),
           "developed-minor FEN parses");

    const float repeat_early = policy_for(evaluator, undeveloped, "f3g5");
    const float repeat_after_development =
        policy_for(evaluator, developed, "f3g5");

    expect(repeat_after_development > repeat_early + 1.7F,
           "quiet repeat move is deprioritised while home minors remain");
}

void test_tactical_minor_moves_are_not_penalised() {
    proton::Evaluator evaluator;
    proton::Position undeveloped;
    proton::Position developed;

    expect(undeveloped.set_fen(
               "4k3/8/8/6r1/8/5N2/8/1NB1KB2 w - - 0 1"),
           "undeveloped tactical FEN parses");
    expect(developed.set_fen(
               "4k3/8/8/6r1/8/5N2/8/4K3 w - - 0 1"),
           "developed tactical FEN parses");

    const float capture_early = policy_for(evaluator, undeveloped, "f3g5");
    const float capture_after_development =
        policy_for(evaluator, developed, "f3g5");

    expect(std::abs(capture_early - capture_after_development) < 0.001F,
           "captures remain exempt from the quiet-development preference");
}

}  // namespace

int main() {
    test_phase_aware_king_policy();
    test_finish_development_before_repeating_a_minor();
    test_tactical_minor_moves_are_not_penalised();

    if (failures != 0) {
        std::cerr << failures << " human-policy test(s) failed\n";
        return 1;
    }
    std::cout << "human-policy tests passed\n";
    return 0;
}
