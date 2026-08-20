#include "eval.h"

#include "attacks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>

namespace proton {
namespace {

constexpr int MaxPhase = 24;
constexpr std::array<int, 6> MgValue = {100, 325, 340, 505, 950, 0};
constexpr std::array<int, 6> EgValue = {120, 305, 325, 525, 930, 0};
constexpr std::array<int, 6> PhaseValue = {0, 1, 1, 2, 4, 0};
constexpr std::array<int, 8> PassedMg = {0, 4, 10, 20, 38, 65, 105, 0};
constexpr std::array<int, 8> PassedEg = {0, 8, 18, 34, 58, 92, 145, 0};

struct TaperedScore {
    int mg = 0;
    int eg = 0;
};

[[nodiscard]] consteval std::array<int, 64> make_centre_distances() {
    std::array<int, 64> distances{};
    for (int square = 0; square < 64; ++square) {
        const int file_twice = 2 * file_of(square) - 7;
        const int rank_twice = 2 * rank_of(square) - 7;
        distances[square] = (file_twice < 0 ? -file_twice : file_twice) +
                            (rank_twice < 0 ? -rank_twice : rank_twice);
    }
    return distances;
}

inline constexpr auto CentreDistances = make_centre_distances();

[[nodiscard]] consteval auto make_piece_square_scores() {
    std::array<std::array<std::array<TaperedScore, 64>, 6>, 2> scores{};
    for (Color color : {White, Black}) {
        for (int square = 0; square < 64; ++square) {
            const int rr = color == White ? rank_of(square) : 7 - rank_of(square);
            const int centre = 14 - CentreDistances[square];
            scores[color][Knight][square] = {
                centre * 4 - (rr == 0 ? 12 : 0), centre * 3};
            scores[color][Bishop][square] = {centre * 2, centre * 2};
            scores[color][Rook][square] = {rr * 2, rr * 3};
            scores[color][Queen][square] = {centre, centre * 2};
            scores[color][King][square] = {-centre * 2, centre * 4};
        }
    }
    return scores;
}

inline constexpr auto PieceSquareScores = make_piece_square_scores();
constexpr std::array<int, 6> MobilityMg = {0, 4, 4, 2, 1, 0};
constexpr std::array<int, 6> MobilityEg = {0, 4, 5, 3, 2, 0};

static_assert(CentreDistances[0] == 14);
static_assert(CentreDistances[27] == 2);
static_assert(PieceSquareScores[White][Knight][1].mg == -4);
static_assert(PieceSquareScores[Black][Rook][56].eg == 0);

// Only pawn, knight, bishop and rook attacks can be materially cheaper
// than a non-king victim. Queen and king attacks can never activate the
// bounded pressure term, so do not maintain redundant per-type maps for them.
constexpr std::size_t ThreatAttackerTypeCount =
    static_cast<std::size_t>(Rook) + 1;
using AttackByType =
    std::array<std::array<Bitboard, ThreatAttackerTypeCount>, 2>;

static_assert(ThreatAttackerTypeCount == 4);

struct ThreatPenalty {
    int mg = 0;
    int eg = 0;
};

[[nodiscard]] consteval std::array<std::array<Bitboard, 64>, 2>
make_passed_pawn_masks() {
    std::array<std::array<Bitboard, 64>, 2> masks{};
    for (Color color : {White, Black}) {
        for (int square = 0; square < 64; ++square) {
            const int file = file_of(square);
            const int start_rank = rank_of(square);
            const int rank_step = color == White ? 1 : -1;
            for (int rank = start_rank + rank_step;
                 rank >= 0 && rank < 8; rank += rank_step) {
                for (int candidate_file = std::max(0, file - 1);
                     candidate_file <= std::min(7, file + 1); ++candidate_file) {
                    masks[color][square] |= bit(rank * 8 + candidate_file);
                }
            }
        }
    }
    return masks;
}

[[nodiscard]] consteval std::array<Bitboard, 64> make_connected_pawn_masks() {
    std::array<Bitboard, 64> masks{};
    for (int square = 0; square < 64; ++square) {
        const int file = file_of(square);
        const int rank = rank_of(square);
        for (int file_step : {-1, 1}) {
            const int candidate_file = file + file_step;
            if (candidate_file < 0 || candidate_file > 7) continue;
            for (int rank_step = -1; rank_step <= 1; ++rank_step) {
                const int candidate_rank = rank + rank_step;
                if (candidate_rank < 0 || candidate_rank > 7) continue;
                masks[square] |= bit(candidate_rank * 8 + candidate_file);
            }
        }
    }
    return masks;
}

inline constexpr auto PassedPawnMasks = make_passed_pawn_masks();
inline constexpr auto ConnectedPawnMasks = make_connected_pawn_masks();

static_assert((PassedPawnMasks[White][8] & bit(17)) != 0);
static_assert((PassedPawnMasks[White][8] & bit(9)) == 0);
static_assert((PassedPawnMasks[Black][55] & bit(46)) != 0);
static_assert((PassedPawnMasks[Black][55] & bit(54)) == 0);
static_assert((ConnectedPawnMasks[8] & (bit(1) | bit(9) | bit(17))) ==
              (bit(1) | bit(9) | bit(17)));

[[nodiscard]] Bitboard piece_attacks(PieceType type, int square, Color color,
                                     Bitboard occupied) {
    switch (type) {
    case Pawn: return attacks::pawn(color, square);
    case Knight: return attacks::Knight[square];
    case Bishop: return attacks::bishop(square, occupied);
    case Rook: return attacks::rook(square, occupied);
    case Queen: return attacks::queen(square, occupied);
    case King: return attacks::King[square];
    default: return 0;
    }
}

[[nodiscard]] int relative_rank(Color color, int square) {
    return color == White ? rank_of(square) : 7 - rank_of(square);
}

[[nodiscard]] int centre_distance(int square) {
    return CentreDistances[square];
}

[[nodiscard]] bool is_passed_pawn(Bitboard enemy_pawns, Color color, int square) {
    return (enemy_pawns & PassedPawnMasks[color][square]) == 0;
}

[[nodiscard]] bool connected_pawn(Bitboard pawns, int square) {
    return (pawns & ConnectedPawnMasks[square]) != 0;
}

[[nodiscard]] int non_pawn_material(const Position& position) {
    int total = 0;
    for (Color color : {White, Black}) {
        for (PieceType type : {Knight, Bishop, Rook, Queen}) {
            total += std::popcount(position.pieces(make_piece(color, type))) *
                     piece_value(type);
        }
    }
    return total;
}

[[nodiscard]] Bitboard home_minor_mask(Color color) {
    if (color == White) {
        return bit(1) | bit(2) | bit(5) | bit(6);
    }
    return bit(57) | bit(58) | bit(61) | bit(62);
}

[[nodiscard]] int least_attacker_value(
    const std::array<Bitboard, ThreatAttackerTypeCount>& attacks_by_type,
    int square) {
    const Bitboard target = bit(square);
    for (PieceType type : {Pawn, Knight, Bishop, Rook}) {
        if ((attacks_by_type[static_cast<std::size_t>(type)] & target) != 0) {
            return piece_value(type);
        }
    }
    return piece_value(King);
}

[[nodiscard]] ThreatPenalty threat_penalty(PieceType victim,
                                           int least_attacker) {
    // Quiescence already resolves executable captures. Pricing loose pieces
    // here double-counts hanging material and makes shallow search unstable.
    // Static evaluation therefore measures only the positional pressure on a
    // defended piece attacked by a materially cheaper unit.
    const int victim_value = piece_value(victim);
    if (least_attacker + piece_value(Pawn) >= victim_value) return {};

    const int exchange_gap = victim_value - least_attacker;
    return {
        std::clamp(exchange_gap / 40, 4, 20),
        std::clamp(exchange_gap / 52, 3, 16),
    };
}

void apply_threat_evaluation(const Position& position,
                             const std::array<Bitboard, 2>& attack_maps,
                             const AttackByType& attacks_by_type,
                             int& mg, int& eg) {
    for (Color color : {White, Black}) {
        const Color enemy = opposite(color);
        const int sign = color == White ? 1 : -1;
        Bitboard threatened = position.occupancy(color) & attack_maps[enemy] &
                              attack_maps[color];
        threatened &= ~position.pieces(make_piece(color, King));

        while (threatened != 0) {
            const int square = static_cast<int>(std::countr_zero(threatened));
            threatened &= threatened - 1;
            const PieceType victim = piece_type(position.piece_at(square));
            if (victim == NoPieceType || victim == Pawn || victim == King) continue;
            const int attacker =
                least_attacker_value(attacks_by_type[enemy], square);
            const ThreatPenalty penalty = threat_penalty(victim, attacker);
            mg -= sign * penalty.mg;
            eg -= sign * penalty.eg;
        }
    }
}

int king_safety(const Position& position, Color color,
                const std::array<std::array<std::uint8_t, 8>, 2>& pawn_files,
                const std::array<Bitboard, 2>& attack_maps) {
    const int king = position.king_square(color);
    if (king == NoSquare) return -500;

    const int file = file_of(king);
    const int rank = rank_of(king);
    const int forward = color == White ? 1 : -1;
    const Piece pawn = make_piece(color, Pawn);
    int safety = 0;

    for (int df = -1; df <= 1; ++df) {
        const int shield_file = file + df;
        if (shield_file < 0 || shield_file > 7) continue;
        bool found = false;
        for (int distance = 1; distance <= 2; ++distance) {
            const int shield_rank = rank + forward * distance;
            if (shield_rank < 0 || shield_rank > 7) continue;
            if (position.piece_at(shield_rank * 8 + shield_file) == pawn) {
                safety += distance == 1 ? 13 : 6;
                found = true;
                break;
            }
        }
        if (!found && pawn_files[color][shield_file] == 0) safety -= 12;
    }

    if ((color == White && (king == 6 || king == 2)) ||
        (color == Black && (king == 62 || king == 58))) {
        safety += 18;
    }

    const Bitboard ring = attacks::King[king] | bit(king);
    const int ring_attacks = std::popcount(attack_maps[opposite(color)] & ring);
    safety -= ring_attacks * 7;

    // Open/semi-open files adjacent to the king are dangerous, especially when
    // the opponent still has heavy pieces.
    const Color enemy = opposite(color);
    const bool enemy_heavy = (position.pieces(make_piece(enemy, Queen)) |
                              position.pieces(make_piece(enemy, Rook))) != 0;
    if (enemy_heavy) {
        for (int df = -1; df <= 1; ++df) {
            const int king_file = file + df;
            if (king_file < 0 || king_file > 7) continue;
            if (pawn_files[color][king_file] == 0) safety -= 8;
            if (pawn_files[color][king_file] == 0 &&
                pawn_files[enemy][king_file] == 0) {
                safety -= 5;
            }
        }
    }
    return safety;
}

[[nodiscard]] int capture_gain(const Position& position, const Move& move) {
    Piece victim = position.piece_at(move.to);
    if ((move.flags & MoveEnPassant) != 0) {
        victim = make_piece(opposite(position.side_to_move()), Pawn);
    }
    int gain = piece_value(piece_type(victim));
    if (move.is_promotion()) gain += piece_value(move.promotion) - piece_value(Pawn);
    return gain;
}

}  // namespace

CoreEvalNet::CoreEvalNet() : pawn_cache_(1U << 14) {}

void CoreEvalNet::clear_cache() {
    for (PawnCacheEntry& entry : pawn_cache_) entry.valid = false;
}

const CoreEvalNet::PawnCacheEntry& CoreEvalNet::pawn_info(
    const Position& position) const {
    const std::size_t index = static_cast<std::size_t>(position.pawn_key()) &
                              (pawn_cache_.size() - 1);
    PawnCacheEntry& entry = pawn_cache_[index];
    if (entry.valid && entry.key == position.pawn_key()) return entry;

    entry = PawnCacheEntry{};
    entry.key = position.pawn_key();
    entry.valid = true;
    entry.attacks = {
        attacks::pawn_map(position.pieces(WhitePawn), White),
        attacks::pawn_map(position.pieces(BlackPawn), Black)};

    for (Color color : {White, Black}) {
        const int sign = color == White ? 1 : -1;
        const Bitboard all_pawns = position.pieces(make_piece(color, Pawn));
        const Bitboard enemy_pawns =
            position.pieces(make_piece(opposite(color), Pawn));
        Bitboard pawns = all_pawns;
        while (pawns != 0) {
            const int square = static_cast<int>(std::countr_zero(pawns));
            pawns &= pawns - 1;
            const int rr = relative_rank(color, square);
            const int centre = 14 - centre_distance(square);
            ++entry.files[color][file_of(square)];

            int mg_square = rr * 6 + std::max(0, centre) / 3;
            int eg_square = rr * 11 + std::max(0, centre) / 5;
            if (file_of(square) == 3 || file_of(square) == 4) mg_square += 4;
            entry.mg += sign * (MgValue[Pawn] + mg_square);
            entry.eg += sign * (EgValue[Pawn] + eg_square);
        }

        for (int file = 0; file < 8; ++file) {
            const int count = entry.files[color][file];
            if (count > 1) {
                entry.mg -= sign * 11 * (count - 1);
                entry.eg -= sign * 17 * (count - 1);
            }
            if (count != 0) {
                const bool isolated =
                    (file == 0 || entry.files[color][file - 1] == 0) &&
                    (file == 7 || entry.files[color][file + 1] == 0);
                if (isolated) {
                    entry.mg -= sign * 10 * count;
                    entry.eg -= sign * 13 * count;
                }
            }
        }

        pawns = all_pawns;
        while (pawns != 0) {
            const int square = static_cast<int>(std::countr_zero(pawns));
            pawns &= pawns - 1;
            const int rr = relative_rank(color, square);
            const bool connected = connected_pawn(all_pawns, square);
            if (is_passed_pawn(enemy_pawns, color, square)) {
                entry.passed[color] |= bit(square);
                int mg_bonus = PassedMg[rr];
                int eg_bonus = PassedEg[rr];
                if (connected) {
                    mg_bonus += 7 + rr * 2;
                    eg_bonus += 10 + rr * 3;
                }
                entry.mg += sign * mg_bonus;
                entry.eg += sign * eg_bonus;
            } else if (connected) {
                entry.mg += sign * 4;
                entry.eg += sign * 6;
            }
        }
    }
    return entry;
}

int CoreEvalNet::evaluate(const Position& position) const {
    const PawnCacheEntry& pawn = pawn_info(position);
    int mg = pawn.mg;
    int eg = pawn.eg;
    int phase = 0;
    int white_material =
        std::popcount(position.pieces(WhitePawn)) * EgValue[Pawn];
    int black_material =
        std::popcount(position.pieces(BlackPawn)) * EgValue[Pawn];
    std::array<int, 2> bishops{};
    const auto& pawn_files = pawn.files;
    const auto& pawn_attacks = pawn.attacks;
    std::array<Bitboard, 2> attack_maps = pawn_attacks;
    AttackByType attacks_by_type{};
    attacks_by_type[White][static_cast<std::size_t>(Pawn)] =
        pawn_attacks[White];
    attacks_by_type[Black][static_cast<std::size_t>(Pawn)] =
        pawn_attacks[Black];
    const Bitboard occupied = position.occupancy_all();

    for (Color color : {White, Black}) {
        const int sign = color == White ? 1 : -1;
        const Bitboard own = position.occupancy(color);
        for (int type_index = Knight; type_index <= King; ++type_index) {
            const PieceType type = static_cast<PieceType>(type_index);
            Bitboard remaining = position.pieces(make_piece(color, type));
            while (remaining != 0) {
                const int square = static_cast<int>(std::countr_zero(remaining));
                remaining &= remaining - 1;
                mg += sign * MgValue[type];
                eg += sign * EgValue[type];
                phase += PhaseValue[type];
                if (type != King) {
                    if (color == White) white_material += EgValue[type];
                    else black_material += EgValue[type];
                }

                if (type == Bishop) ++bishops[color];
                int mg_square = PieceSquareScores[color][type][square].mg;
                int eg_square = PieceSquareScores[color][type][square].eg;

                const Bitboard piece_map =
                    piece_attacks(type, square, color, occupied);
                attack_maps[color] |= piece_map;
                if (type <= Rook) {
                    attacks_by_type[color][static_cast<std::size_t>(type)] |=
                        piece_map;
                }
                Bitboard mobility_map = piece_map & ~own;
                if (type == Knight || type == Bishop) {
                    mobility_map &= ~pawn_attacks[opposite(color)];
                }
                const int mobility = std::popcount(mobility_map);
                mg_square += mobility * MobilityMg[type];
                eg_square += mobility * MobilityEg[type];

                mg += sign * mg_square;
                eg += sign * eg_square;
            }
        }
    }

    phase = std::min(phase, MaxPhase);
    apply_threat_evaluation(position, attack_maps, attacks_by_type, mg, eg);

    for (Color color : {White, Black}) {
        const int sign = color == White ? 1 : -1;
        if (bishops[color] >= 2) {
            mg += sign * 34;
            eg += sign * 48;
        }

        Bitboard passers = pawn.passed[color];
        while (passers != 0) {
            const int square = static_cast<int>(std::countr_zero(passers));
            passers &= passers - 1;
            const int front = square + (color == White ? 8 : -8);
            if (attacks::on_board(front) && position.piece_at(front) != Empty) {
                mg -= sign * 7;
                eg -= sign * 12;
            }

            // A rook directly behind its own passer supports every advance
            // while staying active. The sliding ray also ensures that no
            // intervening piece can earn the bonus.
            Bitboard supporting_rooks =
                attacks::rook(square, occupied) &
                position.pieces(make_piece(color, Rook));
            while (supporting_rooks != 0) {
                const int rook_square =
                    static_cast<int>(std::countr_zero(supporting_rooks));
                supporting_rooks &= supporting_rooks - 1;
                if (relative_rank(color, rook_square) <
                    relative_rank(color, square)) {
                    mg += sign * 8;
                    eg += sign * 18;
                }
            }
        }

        for (PieceType type : {Knight, Bishop}) {
            Bitboard pieces = position.pieces(make_piece(color, type));
            while (pieces != 0) {
                const int square = static_cast<int>(std::countr_zero(pieces));
                pieces &= pieces - 1;
                const int rr = relative_rank(color, square);
                if (rr >= 3 && rr <= 5 &&
                    (pawn_attacks[color] & bit(square)) != 0 &&
                    (pawn_attacks[opposite(color)] & bit(square)) == 0) {
                    mg += sign * (type == Knight ? 18 : 10);
                    eg += sign * (type == Knight ? 12 : 8);
                }
            }
        }

        Bitboard rooks = position.pieces(make_piece(color, Rook));
        while (rooks != 0) {
            const int square = static_cast<int>(std::countr_zero(rooks));
            rooks &= rooks - 1;
            const int file = file_of(square);
            const int enemy_pawns = pawn_files[opposite(color)][file];
            if (pawn_files[color][file] == 0) {
                mg += sign * (enemy_pawns == 0 ? 18 : 10);
                eg += sign * (enemy_pawns == 0 ? 10 : 6);
            }
            if (relative_rank(color, square) == 6) {
                mg += sign * 14;
                eg += sign * 22;
            }
        }
    }

    mg += king_safety(position, White, pawn_files, attack_maps);
    mg -= king_safety(position, Black, pawn_files, attack_maps);

    // Conversion guidance in low-material positions: bring the winning king
    // closer and drive the losing king away from the centre.
    const int material_diff = white_material - black_material;
    if (std::abs(material_diff) >= 250 &&
        white_material + black_material < 2600) {
        const Color winner = material_diff > 0 ? White : Black;
        const Color loser = opposite(winner);
        const int winner_king = position.king_square(winner);
        const int loser_king = position.king_square(loser);
        if (winner_king != NoSquare && loser_king != NoSquare) {
            const int edge = centre_distance(loser_king);
            const int king_distance =
                std::abs(file_of(winner_king) - file_of(loser_king)) +
                std::abs(rank_of(winner_king) - rank_of(loser_king));
            const int mop_up = edge * 3 + (14 - king_distance) * 2;
            eg += winner == White ? mop_up : -mop_up;
        }
    }

    // A small tempo term helps move ordering and reduces horizon oscillation.
    constexpr int Tempo = 10;
    mg += position.side_to_move() == White ? Tempo : -Tempo;
    eg += position.side_to_move() == White ? Tempo / 2 : -Tempo / 2;

    const int score = (mg * phase + eg * (MaxPhase - phase)) / MaxPhase;
    return position.side_to_move() == White ? score : -score;
}

DeepEvalNet::DeepEvalNet(Backend backend) : backend_(backend) {}

bool DeepEvalNet::available() const {
    // Do not advertise a neural/GPU evaluator until a real network loader and
    // validated inference path are present.
    return false;
}

int DeepEvalNet::evaluate(const Position&) const { return 0; }

std::vector<float> DeepEvalNet::score_moves(
    const Position&, const std::vector<Move>& moves) const {
    return std::vector<float>(moves.size(), 0.0F);
}

Evaluator::Evaluator() : cache_(1U << 16) {}

void Evaluator::set_options(const EngineOptions& options) {
    options_ = options;
    deep_ = DeepEvalNet(options.backend);
    clear_cache();
}

void Evaluator::clear_cache() {
    for (CacheEntry& entry : cache_) entry.valid = false;
    core_.clear_cache();
}

int Evaluator::evaluate(const Position& position, bool deep_hint) const {
    const std::size_t index =
        static_cast<std::size_t>(position.key()) & (cache_.size() - 1);
    CacheEntry& entry = cache_[index];
    if (entry.valid && entry.key == position.key()) return entry.score;

    int score = core_.evaluate(position);
    if (deep_hint && deep_.available()) score += deep_.evaluate(position);
    entry.key = position.key();
    entry.score = score;
    entry.valid = true;
    return score;
}

std::vector<float> Evaluator::policy_scores(
    const Position& position, const std::vector<Move>& moves) const {
    std::vector<float> scores;
    scores.reserve(moves.size());
    Position copy = position;

    const Color side = position.side_to_move();
    const bool early_opening = position.fullmove_number() <= 10;
    const bool king_endgame =
        (position.pieces(WhiteQueen) | position.pieces(BlackQueen)) == 0 &&
        non_pawn_material(position) <= 2200;
    const Bitboard home_minors =
        (position.pieces(make_piece(side, Knight)) |
         position.pieces(make_piece(side, Bishop))) &
        home_minor_mask(side);
    const int undeveloped_minors = std::popcount(home_minors);

    for (const Move& move : moves) {
        const Piece piece = position.piece_at(move.from);
        const PieceType type = piece_type(piece);
        const float centrality =
            static_cast<float>(14 - centre_distance(move.to));
        float score = 0.0F;

        // Centralisation is natural for every piece except a middlegame king.
        // In reduced material, switch the king back to an active endgame role.
        if (type == King) {
            score += centrality * (king_endgame ? 0.24F : -0.10F);
        } else {
            score += centrality * 0.18F;
        }

        if (move.is_capture()) {
            const int attacker = piece_value(type);
            score += static_cast<float>(capture_gain(position, move)) * 0.018F;
            score -= static_cast<float>(attacker) * 0.002F;
        }
        if (move.is_promotion()) {
            score += 8.0F +
                     static_cast<float>(piece_value(move.promotion)) * 0.006F;
        }
        if (move.is_castle()) score += king_endgame ? 1.5F : 5.0F;

        const bool home_knight =
            (piece == WhiteKnight && (move.from == 1 || move.from == 6)) ||
            (piece == BlackKnight && (move.from == 57 || move.from == 62));
        const bool home_bishop =
            (piece == WhiteBishop && (move.from == 2 || move.from == 5)) ||
            (piece == BlackBishop && (move.from == 58 || move.from == 61));
        if (home_knight) {
            const int destination_file = file_of(move.to);
            if (destination_file >= 2 && destination_file <= 5) score += 2.8F;
            else score -= 0.9F;  // Do not mistake Na3/Nh3 for normal development.
        }
        if (home_bishop) score += 1.8F;

        // Humans normally finish developing before spending another quiet
        // tempo on an already-developed minor. Tactical captures are exempt.
        if (early_opening && undeveloped_minors > 0 &&
            (type == Knight || type == Bishop) &&
            !home_knight && !home_bishop &&
            !move.is_capture() && !move.is_promotion()) {
            score -= std::min(1.8F,
                              static_cast<float>(undeveloped_minors) * 0.6F);
        }

        if (early_opening && type == Pawn) {
            const int source_file = file_of(move.from);
            if (source_file == 3 || source_file == 4) {
                score += (move.flags & MoveDoublePush) != 0 ? 2.2F : 1.0F;
            } else if (source_file == 0 || source_file == 7) {
                score -= (move.flags & MoveDoublePush) != 0 ? 1.8F : 0.8F;
            } else if ((source_file == 1 || source_file == 6) &&
                       (move.flags & MoveDoublePush) != 0) {
                score -= 0.5F;
            }
        }
        if (type == Queen && position.fullmove_number() <= 7) score -= 1.5F;

        UndoState undo;
        if (copy.make_move(move, undo)) {
            if (copy.in_check(copy.side_to_move())) score += 2.0F;
            copy.unmake_move(move, undo);
        }
        scores.push_back(score);
    }
    return scores;
}

bool Evaluator::deep_available() const { return deep_.available(); }

}  // namespace proton
