#include "eval.h"

#include <algorithm>
#include <array>
#include <cstdlib>

namespace proton {

namespace {

constexpr std::array<int, 64> PawnTable = {
     0,   0,   0,   0,   0,   0,   0,   0,
     5,  10,  10, -20, -20,  10,  10,   5,
     5,  -5, -10,   0,   0, -10,  -5,   5,
     0,   0,   0,  20,  20,   0,   0,   0,
     5,   5,  10,  25,  25,  10,   5,   5,
    10,  10,  20,  30,  30,  20,  10,  10,
    50,  50,  50,  50,  50,  50,  50,  50,
     0,   0,   0,   0,   0,   0,   0,   0
};

constexpr std::array<int, 64> KnightTable = {
    -50, -40, -30, -30, -30, -30, -40, -50,
    -40, -20,   0,   5,   5,   0, -20, -40,
    -30,   5,  10,  15,  15,  10,   5, -30,
    -30,   0,  15,  20,  20,  15,   0, -30,
    -30,   5,  15,  20,  20,  15,   5, -30,
    -30,   0,  10,  15,  15,  10,   0, -30,
    -40, -20,   0,   0,   0,   0, -20, -40,
    -50, -40, -30, -30, -30, -30, -40, -50
};

constexpr std::array<int, 64> BishopTable = {
    -20, -10, -10, -10, -10, -10, -10, -20,
    -10,   5,   0,   0,   0,   0,   5, -10,
    -10,  10,  10,  10,  10,  10,  10, -10,
    -10,   0,  10,  10,  10,  10,   0, -10,
    -10,   5,   5,  10,  10,   5,   5, -10,
    -10,   0,   5,  10,  10,   5,   0, -10,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -20, -10, -10, -10, -10, -10, -10, -20
};

constexpr std::array<int, 64> RookTable = {
      0,   0,   0,   5,   5,   0,   0,   0,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
     -5,   0,   0,   0,   0,   0,   0,  -5,
      5,  10,  10,  10,  10,  10,  10,   5,
      0,   0,   0,   0,   0,   0,   0,   0
};

constexpr std::array<int, 64> QueenTable = {
    -20, -10, -10,  -5,  -5, -10, -10, -20,
    -10,   0,   0,   0,   0,   0,   0, -10,
    -10,   0,   5,   5,   5,   5,   0, -10,
     -5,   0,   5,   5,   5,   5,   0,  -5,
      0,   0,   5,   5,   5,   5,   0,  -5,
    -10,   5,   5,   5,   5,   5,   0, -10,
    -10,   0,   5,   0,   0,   0,   0, -10,
    -20, -10, -10,  -5,  -5, -10, -10, -20
};

constexpr std::array<int, 64> KingTable = {
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -30, -40, -40, -50, -50, -40, -40, -30,
    -20, -30, -30, -40, -40, -30, -30, -20,
    -10, -20, -20, -20, -20, -20, -20, -10,
     20,  20,   0,   0,   0,   0,  20,  20,
     20,  30,  10,   0,   0,  10,  30,  20
};

int mirror_square(int square) {
    return square ^ 56;
}

int piece_square_bonus(Piece piece, int square) {
    const int idx = piece_color(piece) == White ? square : mirror_square(square);
    switch (piece_type(piece)) {
    case Pawn: return PawnTable[idx];
    case Knight: return KnightTable[idx];
    case Bishop: return BishopTable[idx];
    case Rook: return RookTable[idx];
    case Queen: return QueenTable[idx];
    case King: return KingTable[idx];
    default: return 0;
    }
}

}  // namespace

int CoreEvalNet::evaluate(const Position& position) const {
    int score = 0;
    int white_bishops = 0;
    int black_bishops = 0;
    int white_developed_minors = 0;
    int black_developed_minors = 0;
    int total_non_pawn_material = 0;
    std::array<int, 8> white_pawn_files{};
    std::array<int, 8> black_pawn_files{};

    for (int square = 0; square < 64; ++square) {
        const Piece piece = position.piece_at(square);
        if (piece == Empty) {
            continue;
        }
        const int sign = piece_color(piece) == White ? 1 : -1;
        score += sign * (piece_value(piece_type(piece)) + piece_square_bonus(piece, square));

        if (piece_type(piece) != Pawn && piece_type(piece) != King) {
            total_non_pawn_material += piece_value(piece_type(piece));
        }
        if (piece == WhiteBishop) {
            ++white_bishops;
        } else if (piece == BlackBishop) {
            ++black_bishops;
        } else if (piece == WhitePawn) {
            ++white_pawn_files[file_of(square)];
        } else if (piece == BlackPawn) {
            ++black_pawn_files[file_of(square)];
        }

        if (piece == WhiteKnight && square != 1 && square != 6) {
            ++white_developed_minors;
        } else if (piece == WhiteBishop && square != 2 && square != 5) {
            ++white_developed_minors;
        } else if (piece == BlackKnight && square != 57 && square != 62) {
            ++black_developed_minors;
        } else if (piece == BlackBishop && square != 58 && square != 61) {
            ++black_developed_minors;
        }
    }

    for (int square = 0; square < 64; ++square) {
        const Piece piece = position.piece_at(square);
        if (piece == Empty || piece_type(piece) == King) {
            continue;
        }

        const Color color = piece_color(piece);
        const bool attacked = position.is_square_attacked(square, opposite(color));
        const bool defended = position.is_square_attacked(square, color);
        if (attacked && !defended) {
            const int penalty = piece_value(piece_type(piece)) / 7;
            score += color == White ? -penalty : penalty;
        }

        if (piece_type(piece) == Pawn) {
            const int file = file_of(square);
            const int rank = rank_of(square);
            bool passed = true;
            if (color == White) {
                for (int other = square + 8; other < 64 && passed; other += 8) {
                    for (int test_file = std::max(0, file - 1); test_file <= std::min(7, file + 1); ++test_file) {
                        const Piece blocker = position.piece_at(rank_of(other) * 8 + test_file);
                        if (blocker == BlackPawn) {
                            passed = false;
                            break;
                        }
                    }
                }
                if (passed) {
                    score += 10 + rank * 6;
                }
            } else {
                for (int other = square - 8; other >= 0 && passed; other -= 8) {
                    for (int test_file = std::max(0, file - 1); test_file <= std::min(7, file + 1); ++test_file) {
                        const Piece blocker = position.piece_at(rank_of(other) * 8 + test_file);
                        if (blocker == WhitePawn) {
                            passed = false;
                            break;
                        }
                    }
                }
                if (passed) {
                    score -= 10 + (7 - rank) * 6;
                }
            }
        }
    }

    if (white_bishops >= 2) {
        score += 30;
    }
    if (black_bishops >= 2) {
        score -= 30;
    }

    for (int file = 0; file < 8; ++file) {
        if (white_pawn_files[file] > 1) {
            score -= 10 * (white_pawn_files[file] - 1);
        }
        if (black_pawn_files[file] > 1) {
            score += 10 * (black_pawn_files[file] - 1);
        }

        const bool white_isolated = white_pawn_files[file] > 0 &&
            (file == 0 || white_pawn_files[file - 1] == 0) &&
            (file == 7 || white_pawn_files[file + 1] == 0);
        const bool black_isolated = black_pawn_files[file] > 0 &&
            (file == 0 || black_pawn_files[file - 1] == 0) &&
            (file == 7 || black_pawn_files[file + 1] == 0);

        if (white_isolated) {
            score -= 12 * white_pawn_files[file];
        }
        if (black_isolated) {
            score += 12 * black_pawn_files[file];
        }
    }

    score += 12 * (white_developed_minors - black_developed_minors);

    for (int square : {27, 28}) {
        const Piece piece = position.piece_at(square);
        if (piece == WhitePawn) {
            score += 18;
        } else if (piece == WhiteKnight || piece == WhiteBishop) {
            score += 10;
        } else if (piece == BlackPawn) {
            score -= 18;
        } else if (piece == BlackKnight || piece == BlackBishop) {
            score -= 10;
        }
    }
    for (int square : {35, 36}) {
        const Piece piece = position.piece_at(square);
        if (piece == WhitePawn) {
            score += 18;
        } else if (piece == WhiteKnight || piece == WhiteBishop) {
            score += 10;
        } else if (piece == BlackPawn) {
            score -= 18;
        } else if (piece == BlackKnight || piece == BlackBishop) {
            score -= 10;
        }
    }

    const bool opening_phase = total_non_pawn_material > 3200;
    if (opening_phase) {
        if (position.piece_at(3) != WhiteQueen && white_developed_minors < 2) {
            score -= 20;
        }
        if (position.piece_at(59) != BlackQueen && black_developed_minors < 2) {
            score += 20;
        }
    }

    for (int square = 0; square < 64; ++square) {
        const Piece piece = position.piece_at(square);
        if (piece == Empty) {
            continue;
        }

        if (piece_type(piece) == Rook) {
            const int file = file_of(square);
            const bool own_open = piece_color(piece) == White ? white_pawn_files[file] == 0 : black_pawn_files[file] == 0;
            const bool fully_open = white_pawn_files[file] == 0 && black_pawn_files[file] == 0;
            int bonus = 0;
            if (fully_open) {
                bonus = 16;
            } else if (own_open) {
                bonus = 8;
            }
            score += piece_color(piece) == White ? bonus : -bonus;
        }

        if (opening_phase && piece_type(piece) == Queen) {
            const Color color = piece_color(piece);
            const int home_square = color == White ? 3 : 59;
            const int home_rank = color == White ? 0 : 7;
            const bool home_king = position.king_square(color) == (color == White ? 4 : 60);
            const int developed = color == White ? white_developed_minors : black_developed_minors;
            if (square != home_square && developed < 3 && home_king) {
                const int rank_progress = color == White ? rank_of(square) - home_rank : home_rank - rank_of(square);
                const int penalty = 10 + std::max(rank_progress, 0) * 4;
                score += color == White ? -penalty : penalty;
            }
        }
    }

    const int white_king = position.king_square(White);
    const int black_king = position.king_square(Black);
    if (white_king == 6 || white_king == 2) {
        score += 25;
    } else if (opening_phase && white_developed_minors >= 3) {
        score -= 15;
    }
    if (black_king == 62 || black_king == 58) {
        score -= 25;
    } else if (opening_phase && black_developed_minors >= 3) {
        score += 15;
    }

    const auto king_shield = [&](Color color, int king_square) {
        if (king_square == NoSquare) {
            return 0;
        }
        const int rank = rank_of(king_square);
        const int file = file_of(king_square);
        const int forward = color == White ? 1 : -1;
        int shield = 0;
        for (int df = -1; df <= 1; ++df) {
            const int ff = file + df;
            const int rr = rank + forward;
            if (ff < 0 || ff > 7 || rr < 0 || rr > 7) {
                continue;
            }
            const Piece piece = position.piece_at(rr * 8 + ff);
            if (piece == (color == White ? WhitePawn : BlackPawn)) {
                shield += 8;
            }
        }
        return shield;
    };

    score += king_shield(White, white_king);
    score -= king_shield(Black, black_king);

    return position.side_to_move() == White ? score : -score;
}

DeepEvalNet::DeepEvalNet(Backend backend) : backend_(backend) {}

bool DeepEvalNet::available() const {
    return backend_ == Backend::Gpu || backend_ == Backend::Hybrid;
}

int DeepEvalNet::evaluate(const Position& position) const {
    int center_control = 0;
    for (int square : {27, 28, 35, 36}) {
        const Piece piece = position.piece_at(square);
        if (piece == Empty) {
            continue;
        }
        center_control += piece_color(piece) == position.side_to_move() ? 8 : -8;
    }
    return center_control;
}

std::vector<float> DeepEvalNet::score_moves(const Position&, const std::vector<Move>& moves) const {
    std::vector<float> out;
    out.reserve(moves.size());
    for (const Move& move : moves) {
        const int to_file = file_of(move.to);
        const int to_rank = rank_of(move.to);
        const float center_bias = 3.5F - static_cast<float>(std::abs(to_file - 3) + std::abs(to_rank - 3));
        const float promo_bonus = move.is_promotion() ? 2.0F : 0.0F;
        const float capture_bonus = move.is_capture() ? 1.0F : 0.0F;
        out.push_back(center_bias + promo_bonus + capture_bonus);
    }
    return out;
}

std::vector<float> fallback_policy_scores(const Position& position, const std::vector<Move>& moves) {
    std::vector<float> out;
    out.reserve(moves.size());
    for (const Move& move : moves) {
        const Piece piece = position.piece_at(move.from);
        const int to_file = file_of(move.to);
        const int to_rank = rank_of(move.to);
        float score = 0.0F;
        score += 3.5F - 0.75F * static_cast<float>(std::abs(to_file - 3) + std::abs(to_rank - 3));
        if (move.is_capture()) {
            score += 3.0F;
        }
        if (move.is_promotion()) {
            score += 5.0F;
        }
        if ((move.flags & MoveKingCastle) != 0 || (move.flags & MoveQueenCastle) != 0) {
            score += 4.0F;
        }
        if ((piece == WhiteKnight && (move.from == 1 || move.from == 6)) ||
            (piece == BlackKnight && (move.from == 57 || move.from == 62))) {
            score += 2.0F;
        }
        if ((piece == WhiteBishop && (move.from == 2 || move.from == 5)) ||
            (piece == BlackBishop && (move.from == 58 || move.from == 61))) {
            score += 1.5F;
        }
        out.push_back(score);
    }
    return out;
}

void Evaluator::set_options(const EngineOptions& options) {
    options_ = options;
    deep_ = DeepEvalNet(options.backend);
}

int Evaluator::evaluate(const Position& position, bool deep_hint) const {
    int score = core_.evaluate(position);
    if (deep_hint && deep_.available()) {
        score += deep_.evaluate(position);
    }
    return score;
}

std::vector<float> Evaluator::policy_scores(const Position& position, const std::vector<Move>& moves) const {
    if (!deep_.available()) {
        return fallback_policy_scores(position, moves);
    }
    std::vector<float> scores = deep_.score_moves(position, moves);
    std::vector<float> fallback = fallback_policy_scores(position, moves);
    for (std::size_t index = 0; index < scores.size(); ++index) {
        scores[index] += 0.35F * fallback[index];
    }
    return scores;
}

bool Evaluator::deep_available() const {
    return deep_.available();
}

}  // namespace proton
