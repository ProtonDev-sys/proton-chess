#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "engine/human_prior.h"

namespace {

// Goldens come from the original Python candidate_features, never this port.
// Their float32 rows are dotted with JSON double weights, then cast to float32.
constexpr float Tolerance = 3.0e-6F;
int failures = 0;

void expect(bool condition, const std::string& message) {
    if (!condition) {
        if (failures < 30) std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

struct Fixture {
    std::string label;
    std::string fen;
    std::map<std::string, float> expected;
    std::map<std::string, float> actual;
};

std::vector<Fixture> load_fixtures(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open golden fixture: " + path.string());
    std::vector<Fixture> fixtures;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line.empty() || line.front() == '#') continue;
        const std::string context = path.string() + ':' + std::to_string(line_number);
        const auto first = line.find('|');
        const auto second = first == std::string::npos ? first : line.find('|', first + 1);
        if (first == std::string::npos || second == std::string::npos)
            throw std::runtime_error("malformed fixture at " + context);
        Fixture fixture{line.substr(0, first), line.substr(first + 1, second - first - 1), {}, {}};
        std::istringstream tokens(line.substr(second + 1));
        std::string token;
        while (tokens >> token) {
            const auto equals = token.find('=');
            if (equals == std::string::npos)
                throw std::runtime_error("malformed move/logit at " + context);
            const std::string uci = token.substr(0, equals);
            const std::string number = token.substr(equals + 1);
            std::size_t used = 0;
            const float value = std::stof(number, &used);
            if ((uci.size() != 4 && uci.size() != 5) || used != number.size() ||
                !std::isfinite(value) || !fixture.expected.emplace(uci, value).second)
                throw std::runtime_error("invalid or duplicate golden move at " + context);
        }
        fixtures.push_back(std::move(fixture));
    }
    if (!input.eof()) throw std::runtime_error("error reading " + path.string());
    return fixtures;
}

struct Coverage {
    std::size_t candidates = 0;
    int checks = 0;
    int terminals = 0;
    int ep = 0;
    int random = 0;
    int captures = 0;
    std::array<std::array<bool, 2>, 2> castles{};
    std::array<std::array<bool, 4>, 2> promotions{};
    float minimum = std::numeric_limits<float>::infinity();
    float maximum = -std::numeric_limits<float>::infinity();
    float max_error = 0.0F;
};

void test_fixture(Fixture& fixture, Coverage& coverage) {
    proton::Position position;
    const std::string context = fixture.label + " [" + fixture.fen + ']';
    if (!position.set_fen(fixture.fen)) {
        expect(false, "FEN parses: " + context);
        return;
    }
    expect(!position.in_check(proton::opposite(position.side_to_move())),
           "previous mover's king is safe: " + context);
    const std::string before_fen = position.fen();
    const auto before_key = position.key();
    const auto before_pawn_key = position.pawn_key();
    auto preserved = [&]() {
        expect(position.fen() == before_fen && position.key() == before_key &&
                   position.pawn_key() == before_pawn_key,
               "input FEN, key and pawn key preserved: " + context);
    };
    std::vector<proton::Move> legal;
    position.generate_legal_moves(legal);
    const auto original_legal = legal;
    std::map<std::string, proton::Move> by_uci;
    for (const auto& move : legal) {
        expect(by_uci.emplace(move.to_uci(), move).second, "unique legal move: " + context);
        const auto expected = fixture.expected.find(move.to_uci());
        expect(expected != fixture.expected.end(), "unexpected legal move " + move.to_uci() + ": " + context);
        expect(position.parse_uci_move(move.to_uci()) == move,
               "parsed move retains flags: " + move.to_uci() + ": " + context);
        const auto color = static_cast<std::size_t>(position.side_to_move());
        if (move.is_castle()) {
            const std::size_t wing = (move.flags & proton::MoveKingCastle) != 0 ? 0 : 1;
            coverage.castles[color][wing] = true;
        }
        if (move.is_promotion()) {
            const auto piece = static_cast<std::size_t>(move.promotion);
            expect(piece >= 1 && piece <= 4, "promotion is N/B/R/Q: " + context);
            if (piece >= 1 && piece <= 4) coverage.promotions[color][piece - 1] = true;
        }
        coverage.ep += (move.flags & proton::MoveEnPassant) != 0;
        coverage.captures += move.is_capture();
    }
    expect(by_uci.size() == fixture.expected.size(), "complete Python legal move set: " + context);
    for (const auto& [uci, value] : fixture.expected) {
        (void)value;
        expect(by_uci.count(uci) == 1, "missing legal move " + uci + ": " + context);
    }
    coverage.candidates += legal.size();
    coverage.checks += position.in_check(position.side_to_move());
    coverage.terminals += legal.empty();
    coverage.random += fixture.label.rfind("random_", 0) == 0;

    const auto scores = proton::human_move_logits(position, legal);
    preserved();
    expect(scores.size() == legal.size(), "one raw logit per move: " + context);
    expect(legal == original_legal, "input move vector preserved: " + context);
    expect(scores == proton::human_move_logits(position, legal), "deterministic raw logits: " + context);
    preserved();
    for (std::size_t i = 0; i < std::min(scores.size(), legal.size()); ++i) {
        const auto uci = legal[i].to_uci();
        expect(std::isfinite(scores[i]), "finite raw logit " + uci + ": " + context);
        fixture.actual.emplace(uci, scores[i]);
        coverage.minimum = std::min(coverage.minimum, scores[i]);
        coverage.maximum = std::max(coverage.maximum, scores[i]);
        const auto expected = fixture.expected.find(uci);
        if (expected == fixture.expected.end()) continue;
        const float error = std::abs(scores[i] - expected->second);
        coverage.max_error = std::max(coverage.max_error, error);
        if (!std::isfinite(scores[i]) || error > Tolerance) {
            std::ostringstream detail;
            detail << std::setprecision(10) << "raw logit " << uci << " expected "
                   << expected->second << ", got " << scores[i] << ", abs error " << error
                   << ": " << context;
            expect(false, detail.str());
        }
    }

    std::reverse(legal.begin(), legal.end());
    const auto reversed = proton::human_move_logits(position, legal);
    expect(reversed.size() == scores.size() &&
               std::equal(scores.begin(), scores.end(), reversed.rbegin(), reversed.rend()),
           "reversed input retains move/logit alignment: " + context);
    preserved();
    expect(proton::human_move_logits(position, {}).empty(), "empty input yields empty output: " + context);
    preserved();
    if (!original_legal.empty() && scores.size() == original_legal.size()) {
        const std::vector<proton::Move> subset = {original_legal.back(), original_legal.front(), original_legal.back()};
        expect(proton::human_move_logits(position, subset) ==
                   std::vector<float>{scores.back(), scores.front(), scores.back()},
               "subset and duplicate candidates retain raw values: " + context);
        expect(proton::human_move_logits(position, {original_legal.front()}) == std::vector<float>{scores.front()},
               "singleton retains raw logit: " + context);
        preserved();
    }
    std::vector<proton::Move> after_legal;
    position.generate_legal_moves(after_legal);
    expect(after_legal == original_legal, "legal moves preserved after inference: " + context);
    preserved();
}

std::string mirror_uci(std::string uci) {
    uci[1] = static_cast<char>('8' - (uci[1] - '1'));
    uci[3] = static_cast<char>('8' - (uci[3] - '1'));
    return uci;
}

void test_mirror_pairs(const std::vector<Fixture>& fixtures) {
    expect(fixtures.size() % 2 == 0, "fixtures contain complete independent Python mirror pairs");
    for (std::size_t i = 0; i + 1 < fixtures.size(); i += 2) {
        const auto& original = fixtures[i];
        const auto& mirrored = fixtures[i + 1];
        expect(mirrored.label == original.label + ".mirror", "adjacent mirror pair: " + original.label);
        proton::Position a, b;
        if (!a.set_fen(original.fen) || !b.set_fen(mirrored.fen)) continue;
        expect(a.side_to_move() == proton::opposite(b.side_to_move()), "mirror side to move: " + original.label);
        for (int square = 0; square < 64; ++square) {
            const auto piece = a.piece_at(square);
            const auto reflected = piece == proton::Empty ? proton::Empty :
                proton::make_piece(proton::opposite(proton::piece_color(piece)), proton::piece_type(piece));
            expect(b.piece_at(square ^ 56) == reflected, "independent mirror board: " + original.label);
        }
        const int ep = a.en_passant_square();
        expect(b.en_passant_square() == (ep == proton::NoSquare ? proton::NoSquare : ep ^ 56),
               "mirror en passant rights: " + original.label);
        expect(b.castling_rights() == ((a.castling_rights() & 3) << 2 | (a.castling_rights() & 12) >> 2),
               "mirror castling rights: " + original.label);
        expect(original.expected.size() == mirrored.expected.size(), "mirror legal move count: " + original.label);
        for (const auto& [uci, value] : original.expected) {
            const auto reflected_uci = mirror_uci(uci);
            const auto expected = mirrored.expected.find(reflected_uci);
            expect(expected != mirrored.expected.end() && expected->second == value,
                   "Python reference symmetry " + uci + ": " + original.label);
            const auto left = original.actual.find(uci);
            const auto right = mirrored.actual.find(reflected_uci);
            expect(left != original.actual.end() && right != mirrored.actual.end() && left->second == right->second,
                   "native color symmetry " + uci + ": " + original.label);
        }
    }
}

void test_coverage(const std::vector<Fixture>& fixtures, const Coverage& coverage) {
    expect(fixtures.size() >= 80 && coverage.candidates >= 1000, "substantial independent golden coverage");
    expect(coverage.random >= 32, "seeded random legal positions and mirrors covered");
    expect(coverage.checks >= 12 && coverage.terminals >= 4, "check evasions, mate and stalemate covered");
    expect(coverage.ep >= 4 && coverage.captures >= 100, "en passant and ordinary captures covered");
    for (std::size_t color = 0; color < 2; ++color) {
        expect(coverage.castles[color][0] && coverage.castles[color][1], "both castling wings for each color");
        expect(std::all_of(coverage.promotions[color].begin(), coverage.promotions[color].end(),
                           [](bool covered) { return covered; }), "all promotion pieces for each color");
    }
    expect(coverage.minimum < -8.0F && coverage.maximum > 10.0F, "raw logits retain negative values and extrema");
    auto named = [&](const std::string& label) -> const Fixture* {
        for (const auto& fixture : fixtures) if (fixture.label == label) return &fixture;
        expect(false, "required curated fixture: " + label);
        return nullptr;
    };
    if (const auto* fixture = named("ep_pinned_illegal"))
        expect(fixture->expected.count("g5f6") == 0, "EP exposing own king is excluded");
    if (const auto* fixture = named("ep_discovered_check_evasion")) {
        proton::Position position;
        position.set_fen(fixture->fen);
        const auto move = position.parse_uci_move("e5d6");
        expect(!move.is_null() && position.in_check(position.side_to_move()) &&
                   (move.flags & proton::MoveEnPassant) != 0 && position.gives_check(move),
               "EP check evasion also gives discovered check");
    }
    for (const auto* label : {"pinned_enemy_attacker", "pinned_own_defender", "dev_exact_22",
                              "dev_below_22", "dev_above_22", "endgame_no_queen_below",
                              "dev_no_queen_above", "material_ahead", "material_behind"})
        (void)named(label);
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const auto path = argc > 1 ? std::filesystem::path(argv[1]) :
            std::filesystem::path(__FILE__).parent_path() / "data/human_prior_golden.txt";
        auto fixtures = load_fixtures(path);
        Coverage coverage;
        for (auto& fixture : fixtures) test_fixture(fixture, coverage);
        test_mirror_pairs(fixtures);
        test_coverage(fixtures, coverage);
        if (failures != 0) {
            std::cerr << failures << " human prior test failures\n";
            return EXIT_FAILURE;
        }
        std::cout << "human prior tests passed: " << fixtures.size() << " positions, "
                  << coverage.candidates << " candidates; max abs error " << coverage.max_error
                  << ", raw range [" << coverage.minimum << ", " << coverage.maximum << "]\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
