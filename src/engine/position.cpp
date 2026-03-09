#include "position.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace proton {

namespace {

constexpr int WhiteKingSide = 1 << 0;
constexpr int WhiteQueenSide = 1 << 1;
constexpr int BlackKingSide = 1 << 2;
constexpr int BlackQueenSide = 1 << 3;

constexpr std::array<int, 8> KnightOffsets = {17, 15, 10, 6, -17, -15, -10, -6};
constexpr std::array<int, 8> KingOffsets = {8, -8, 1, -1, 9, 7, -9, -7};
constexpr std::array<int, 4> BishopDirections = {9, 7, -9, -7};
constexpr std::array<int, 4> RookDirections = {8, -8, 1, -1};

std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

std::uint64_t zobrist_piece(Piece piece, int square) {
    return splitmix64(0x1000ULL + static_cast<std::uint64_t>(piece) * 64ULL + static_cast<std::uint64_t>(square));
}

std::uint64_t zobrist_castling(int rights) {
    return splitmix64(0x2000ULL + static_cast<std::uint64_t>(rights));
}

std::uint64_t zobrist_ep(int file) {
    return splitmix64(0x3000ULL + static_cast<std::uint64_t>(file));
}

std::uint64_t zobrist_side() {
    return splitmix64(0x4000ULL);
}

bool on_board(int square) {
    return square >= 0 && square < 64;
}

bool wraps(int from, int to, int delta) {
    if (!on_board(to)) {
        return true;
    }

    const int from_file = file_of(from);
    const int to_file = file_of(to);
    switch (delta) {
    case 1:
    case -1:
    case 9:
    case -9:
    case 7:
    case -7:
    case 17:
    case -17:
    case 15:
    case -15:
        return std::abs(from_file - to_file) != 1;
    case 10:
    case -10:
    case 6:
    case -6:
        return std::abs(from_file - to_file) != 2;
    default:
        return false;
    }
}

int castling_mask_for_rook_square(int square) {
    switch (square) {
    case 0: return WhiteQueenSide;
    case 7: return WhiteKingSide;
    case 56: return BlackQueenSide;
    case 63: return BlackKingSide;
    default: return 0;
    }
}

}  // namespace

Position::Position() {
    set_startpos();
}

void Position::clear() {
    board_.fill(Empty);
    pieces_.fill(0);
    occupancy_.fill(0);
    stm_ = White;
    castling_ = 0;
    ep_ = NoSquare;
    halfmove_ = 0;
    fullmove_ = 1;
    key_ = 0;
    history_keys_.clear();
    move_history_.clear();
}

void Position::recompute_occupancy() {
    occupancy_[White] = 0;
    occupancy_[Black] = 0;
    for (int piece = WhitePawn; piece <= WhiteKing; ++piece) {
        occupancy_[White] |= pieces_[piece];
    }
    for (int piece = BlackPawn; piece <= BlackKing; ++piece) {
        occupancy_[Black] |= pieces_[piece];
    }
}

void Position::rebuild_bitboards() {
    pieces_.fill(0);
    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        if (piece != Empty) {
            pieces_[piece] |= bit(square);
        }
    }
    recompute_occupancy();
}

void Position::add_piece(Piece piece, int square) {
    board_[square] = piece;
    pieces_[piece] |= bit(square);
    if (piece_color(piece) != NoColor) {
        occupancy_[piece_color(piece)] |= bit(square);
    }
}

void Position::remove_piece(Piece piece, int square) {
    board_[square] = Empty;
    pieces_[piece] &= ~bit(square);
    if (piece_color(piece) != NoColor) {
        occupancy_[piece_color(piece)] &= ~bit(square);
    }
}

void Position::move_piece(Piece piece, int from, int to) {
    remove_piece(piece, from);
    add_piece(piece, to);
}

std::uint64_t Position::compute_key() const {
    std::uint64_t hash = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        if (piece != Empty) {
            hash ^= zobrist_piece(piece, square);
        }
    }
    hash ^= zobrist_castling(castling_);
    if (ep_ != NoSquare) {
        hash ^= zobrist_ep(file_of(ep_));
    }
    if (stm_ == Black) {
        hash ^= zobrist_side();
    }
    return hash;
}

void Position::push_history() {
    history_keys_.push_back(key_);
}

void Position::pop_history() {
    if (!history_keys_.empty()) {
        history_keys_.pop_back();
    }
}

void Position::reset_history() {
    history_keys_.clear();
    history_keys_.push_back(key_);
    move_history_.clear();
}

void Position::set_startpos() {
    set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool Position::set_fen(const std::string& fen_text) {
    clear();

    std::istringstream stream(fen_text);
    std::string board_part;
    std::string stm_part;
    std::string castling_part;
    std::string ep_part;

    if (!(stream >> board_part >> stm_part >> castling_part >> ep_part >> halfmove_ >> fullmove_)) {
        return false;
    }

    int square = 56;
    for (char c : board_part) {
        if (c == '/') {
            square -= 16;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            square += c - '0';
            continue;
        }

        const Piece piece = char_to_piece(c);
        if (piece == Empty || square < 0 || square >= 64) {
            return false;
        }
        add_piece(piece, square);
        ++square;
    }

    stm_ = stm_part == "b" ? Black : White;
    if (castling_part.find('K') != std::string::npos) castling_ |= WhiteKingSide;
    if (castling_part.find('Q') != std::string::npos) castling_ |= WhiteQueenSide;
    if (castling_part.find('k') != std::string::npos) castling_ |= BlackKingSide;
    if (castling_part.find('q') != std::string::npos) castling_ |= BlackQueenSide;
    ep_ = ep_part == "-" ? NoSquare : square_from_string(ep_part);

    rebuild_bitboards();
    key_ = compute_key();
    reset_history();
    return true;
}

std::string Position::fen() const {
    std::string board_text;
    for (int rank = 7; rank >= 0; --rank) {
        int empty = 0;
        for (int file = 0; file < 8; ++file) {
            const Piece piece = board_[rank * 8 + file];
            if (piece == Empty) {
                ++empty;
                continue;
            }
            if (empty > 0) {
                board_text.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            board_text.push_back(piece_to_char(piece));
        }
        if (empty > 0) {
            board_text.push_back(static_cast<char>('0' + empty));
        }
        if (rank > 0) {
            board_text.push_back('/');
        }
    }

    std::string castling_text = "-";
    if (castling_ != 0) {
        castling_text.clear();
        if (castling_ & WhiteKingSide) castling_text.push_back('K');
        if (castling_ & WhiteQueenSide) castling_text.push_back('Q');
        if (castling_ & BlackKingSide) castling_text.push_back('k');
        if (castling_ & BlackQueenSide) castling_text.push_back('q');
    }

    return board_text + (stm_ == White ? " w " : " b ") + castling_text + " " +
           (ep_ == NoSquare ? "-" : square_to_string(ep_)) + " " +
           std::to_string(halfmove_) + " " + std::to_string(fullmove_);
}

int Position::king_square(Color color) const {
    const Piece target = color == White ? WhiteKing : BlackKing;
    for (int square = 0; square < 64; ++square) {
        if (board_[square] == target) {
            return square;
        }
    }
    return NoSquare;
}

bool Position::is_square_attacked(int square, Color by) const {
    const int pawn_left = by == White ? square - 9 : square + 7;
    const int pawn_right = by == White ? square - 7 : square + 9;
    const Piece pawn = by == White ? WhitePawn : BlackPawn;
    if (on_board(pawn_left) && !wraps(square, pawn_left, by == White ? -9 : 7) && board_[pawn_left] == pawn) {
        return true;
    }
    if (on_board(pawn_right) && !wraps(square, pawn_right, by == White ? -7 : 9) && board_[pawn_right] == pawn) {
        return true;
    }

    const Piece knight = by == White ? WhiteKnight : BlackKnight;
    for (int delta : KnightOffsets) {
        const int from = square + delta;
        if (on_board(from) && !wraps(square, from, delta) && board_[from] == knight) {
            return true;
        }
    }

    const Piece bishop = by == White ? WhiteBishop : BlackBishop;
    const Piece rook = by == White ? WhiteRook : BlackRook;
    const Piece queen = by == White ? WhiteQueen : BlackQueen;
    const Piece king = by == White ? WhiteKing : BlackKing;

    for (int delta : BishopDirections) {
        int current = square;
        while (true) {
            const int next = current + delta;
            if (!on_board(next) || wraps(current, next, delta)) {
                break;
            }
            current = next;
            const Piece occupant = board_[current];
            if (occupant == Empty) {
                continue;
            }
            if (occupant == bishop || occupant == queen) {
                return true;
            }
            break;
        }
    }

    for (int delta : RookDirections) {
        int current = square;
        while (true) {
            const int next = current + delta;
            if (!on_board(next) || wraps(current, next, delta)) {
                break;
            }
            current = next;
            const Piece occupant = board_[current];
            if (occupant == Empty) {
                continue;
            }
            if (occupant == rook || occupant == queen) {
                return true;
            }
            break;
        }
    }

    for (int delta : KingOffsets) {
        const int from = square + delta;
        if (on_board(from) && !wraps(square, from, delta) && board_[from] == king) {
            return true;
        }
    }

    return false;
}

bool Position::in_check(Color color) const {
    const int square = king_square(color);
    return square != NoSquare && is_square_attacked(square, opposite(color));
}

bool Position::is_draw_by_material() const {
    int minor_count = 0;
    int bishop_colors = 0;
    int major_or_pawn = 0;

    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        switch (piece_type(piece)) {
        case Pawn:
        case Rook:
        case Queen:
            if (piece != Empty) {
                ++major_or_pawn;
            }
            break;
        case Knight:
            ++minor_count;
            break;
        case Bishop:
            ++minor_count;
            bishop_colors |= ((file_of(square) + rank_of(square)) & 1) ? 1 : 2;
            break;
        default:
            break;
        }
    }

    if (major_or_pawn > 0) {
        return false;
    }
    if (minor_count <= 1) {
        return true;
    }
    return minor_count == 2 && bishop_colors != 3;
}

bool Position::is_repetition() const {
    int count = 0;
    for (std::uint64_t key : history_keys_) {
        if (key == key_) {
            ++count;
        }
    }
    return count >= 3;
}

void Position::generate_pseudo_legal_moves(std::vector<Move>& moves, bool captures_only) const {
    moves.clear();
    const Color us = stm_;
    const Color them = opposite(us);

    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        if (piece == Empty || piece_color(piece) != us) {
            continue;
        }

        switch (piece_type(piece)) {
        case Pawn: {
            const int direction = us == White ? 8 : -8;
            const int start_rank = us == White ? 1 : 6;
            const int promotion_rank = us == White ? 6 : 1;
            const int one_forward = square + direction;

            if (!captures_only && on_board(one_forward) && board_[one_forward] == Empty) {
                if (rank_of(square) == promotion_rank) {
                    for (PieceType promo : {Queen, Rook, Bishop, Knight}) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(one_forward), promo, static_cast<std::uint8_t>(MovePromotion)});
                    }
                } else {
                    moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(one_forward), NoPieceType, MoveQuiet});
                    const int two_forward = square + direction * 2;
                    if (rank_of(square) == start_rank && on_board(two_forward) && board_[two_forward] == Empty) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(two_forward), NoPieceType, MoveDoublePush});
                    }
                }
            }

            for (int capture_delta : {us == White ? 7 : -9, us == White ? 9 : -7}) {
                const int target = square + capture_delta;
                if (!on_board(target) || wraps(square, target, capture_delta)) {
                    continue;
                }
                if (target == ep_) {
                    moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, static_cast<std::uint8_t>(MoveCapture | MoveEnPassant)});
                    continue;
                }
                if (board_[target] != Empty && piece_color(board_[target]) == them) {
                    if (rank_of(square) == promotion_rank) {
                        for (PieceType promo : {Queen, Rook, Bishop, Knight}) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), promo, static_cast<std::uint8_t>(MoveCapture | MovePromotion)});
                        }
                    } else {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, MoveCapture});
                    }
                }
            }
            break;
        }
        case Knight:
            for (int delta : KnightOffsets) {
                const int target = square + delta;
                if (!on_board(target) || wraps(square, target, delta)) {
                    continue;
                }
                if (board_[target] == Empty) {
                    if (!captures_only) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, MoveQuiet});
                    }
                } else if (piece_color(board_[target]) == them) {
                    moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, MoveCapture});
                }
            }
            break;
        case Bishop:
            for (int delta : BishopDirections) {
                int current = square;
                while (true) {
                    const int target = current + delta;
                    if (!on_board(target) || wraps(current, target, delta)) {
                        break;
                    }
                    current = target;
                    if (board_[current] == Empty) {
                        if (!captures_only) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveQuiet});
                        }
                        continue;
                    }
                    if (piece_color(board_[current]) == them) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveCapture});
                    }
                    break;
                }
            }
            break;
        case Rook:
            for (int delta : RookDirections) {
                int current = square;
                while (true) {
                    const int target = current + delta;
                    if (!on_board(target) || wraps(current, target, delta)) {
                        break;
                    }
                    current = target;
                    if (board_[current] == Empty) {
                        if (!captures_only) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveQuiet});
                        }
                        continue;
                    }
                    if (piece_color(board_[current]) == them) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveCapture});
                    }
                    break;
                }
            }
            break;
        case Queen:
            for (int delta : BishopDirections) {
                int current = square;
                while (true) {
                    const int target = current + delta;
                    if (!on_board(target) || wraps(current, target, delta)) {
                        break;
                    }
                    current = target;
                    if (board_[current] == Empty) {
                        if (!captures_only) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveQuiet});
                        }
                        continue;
                    }
                    if (piece_color(board_[current]) == them) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveCapture});
                    }
                    break;
                }
            }
            for (int delta : RookDirections) {
                int current = square;
                while (true) {
                    const int target = current + delta;
                    if (!on_board(target) || wraps(current, target, delta)) {
                        break;
                    }
                    current = target;
                    if (board_[current] == Empty) {
                        if (!captures_only) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveQuiet});
                        }
                        continue;
                    }
                    if (piece_color(board_[current]) == them) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(current), NoPieceType, MoveCapture});
                    }
                    break;
                }
            }
            break;
        case King:
            for (int delta : KingOffsets) {
                const int target = square + delta;
                if (!on_board(target) || wraps(square, target, delta)) {
                    continue;
                }
                if (board_[target] == Empty) {
                    if (!captures_only) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, MoveQuiet});
                    }
                } else if (piece_color(board_[target]) == them) {
                    moves.push_back(Move{static_cast<std::uint8_t>(square), static_cast<std::uint8_t>(target), NoPieceType, MoveCapture});
                }
            }

            if (!captures_only && !in_check(us)) {
                if (us == White) {
                    if ((castling_ & WhiteKingSide) && board_[5] == Empty && board_[6] == Empty &&
                        !is_square_attacked(5, them) && !is_square_attacked(6, them)) {
                        moves.push_back(Move{4, 6, NoPieceType, MoveKingCastle});
                    }
                    if ((castling_ & WhiteQueenSide) && board_[3] == Empty && board_[2] == Empty && board_[1] == Empty &&
                        !is_square_attacked(3, them) && !is_square_attacked(2, them)) {
                        moves.push_back(Move{4, 2, NoPieceType, MoveQueenCastle});
                    }
                } else {
                    if ((castling_ & BlackKingSide) && board_[61] == Empty && board_[62] == Empty &&
                        !is_square_attacked(61, them) && !is_square_attacked(62, them)) {
                        moves.push_back(Move{60, 62, NoPieceType, MoveKingCastle});
                    }
                    if ((castling_ & BlackQueenSide) && board_[59] == Empty && board_[58] == Empty && board_[57] == Empty &&
                        !is_square_attacked(59, them) && !is_square_attacked(58, them)) {
                        moves.push_back(Move{60, 58, NoPieceType, MoveQueenCastle});
                    }
                }
            }
            break;
        default:
            break;
        }
    }
}

void Position::generate_legal_moves(std::vector<Move>& moves) const {
    std::vector<Move> pseudo;
    generate_pseudo_legal_moves(pseudo, false);
    moves.clear();
    moves.reserve(pseudo.size());

    Position copy = *this;
    for (const Move& move : pseudo) {
        UndoState undo;
        if (copy.make_move(move, undo)) {
            moves.push_back(move);
            copy.unmake_move(move, undo);
        }
    }
}

void Position::generate_captures(std::vector<Move>& moves) const {
    std::vector<Move> pseudo;
    generate_pseudo_legal_moves(pseudo, true);
    moves.clear();
    moves.reserve(pseudo.size());

    Position copy = *this;
    for (const Move& move : pseudo) {
        UndoState undo;
        if (copy.make_move(move, undo)) {
            moves.push_back(move);
            copy.unmake_move(move, undo);
        }
    }
}

void Position::generate_pseudo_moves(std::vector<Move>& moves) const {
    generate_pseudo_legal_moves(moves, false);
}

void Position::generate_pseudo_captures(std::vector<Move>& moves) const {
    generate_pseudo_legal_moves(moves, true);
}

Move Position::parse_uci_move(const std::string& text) const {
    std::vector<Move> legal;
    generate_legal_moves(legal);
    for (const Move& move : legal) {
        if (move.to_uci() == text) {
            return move;
        }
    }
    return Move::null();
}

bool Position::make_move_unchecked(const Move& move, UndoState& undo) {
    undo.captured = Empty;
    undo.castling_rights = castling_;
    undo.ep_square = ep_;
    undo.halfmove_clock = halfmove_;
    undo.fullmove_number = fullmove_;
    undo.key = key_;

    const Color us = stm_;
    const Color them = opposite(us);
    const Piece moving = board_[move.from];
    if (moving == Empty) {
        return false;
    }

    Piece captured = board_[move.to];
    ep_ = NoSquare;

    if ((move.flags & MoveEnPassant) != 0) {
        const int captured_square = us == White ? move.to - 8 : move.to + 8;
        captured = board_[captured_square];
        remove_piece(captured, captured_square);
    } else if (captured != Empty) {
        remove_piece(captured, move.to);
    }

    undo.captured = captured;
    remove_piece(moving, move.from);

    Piece placed = moving;
    if ((move.flags & MovePromotion) != 0) {
        placed = us == White
            ? static_cast<Piece>(static_cast<int>(WhitePawn) + static_cast<int>(move.promotion))
            : static_cast<Piece>(static_cast<int>(BlackPawn) + static_cast<int>(move.promotion));
    }
    add_piece(placed, move.to);

    if ((move.flags & MoveKingCastle) != 0) {
        if (us == White) {
            move_piece(WhiteRook, 7, 5);
        } else {
            move_piece(BlackRook, 63, 61);
        }
    } else if ((move.flags & MoveQueenCastle) != 0) {
        if (us == White) {
            move_piece(WhiteRook, 0, 3);
        } else {
            move_piece(BlackRook, 56, 59);
        }
    }

    if ((move.flags & MoveDoublePush) != 0) {
        ep_ = us == White ? move.to - 8 : move.to + 8;
    }

    if (piece_type(moving) == King) {
        castling_ &= us == White ? ~(WhiteKingSide | WhiteQueenSide) : ~(BlackKingSide | BlackQueenSide);
    }
    if (piece_type(moving) == Rook) {
        castling_ &= ~castling_mask_for_rook_square(move.from);
    }
    if (captured != Empty) {
        castling_ &= ~castling_mask_for_rook_square(move.to);
    }

    halfmove_ = (piece_type(moving) == Pawn || captured != Empty) ? 0 : halfmove_ + 1;
    if (us == Black) {
        ++fullmove_;
    }

    stm_ = them;
    key_ = compute_key();
    push_history();
    move_history_.push_back(move);
    return true;
}

bool Position::make_move(const Move& move, UndoState& undo) {
    const Color us = stm_;
    if (!make_move_unchecked(move, undo)) {
        return false;
    }
    if (in_check(us)) {
        unmake_move(move, undo);
        return false;
    }
    return true;
}

void Position::unmake_move(const Move& move, const UndoState& undo) {
    pop_history();
    if (!move_history_.empty()) {
        move_history_.pop_back();
    }
    stm_ = opposite(stm_);
    castling_ = undo.castling_rights;
    ep_ = undo.ep_square;
    halfmove_ = undo.halfmove_clock;
    fullmove_ = undo.fullmove_number;

    const Color us = stm_;
    Piece moved = board_[move.to];
    remove_piece(moved, move.to);

    if ((move.flags & MovePromotion) != 0) {
        moved = us == White ? WhitePawn : BlackPawn;
    }
    add_piece(moved, move.from);

    if ((move.flags & MoveKingCastle) != 0) {
        if (us == White) {
            move_piece(WhiteRook, 5, 7);
        } else {
            move_piece(BlackRook, 61, 63);
        }
    } else if ((move.flags & MoveQueenCastle) != 0) {
        if (us == White) {
            move_piece(WhiteRook, 3, 0);
        } else {
            move_piece(BlackRook, 59, 56);
        }
    }

    if ((move.flags & MoveEnPassant) != 0) {
        const int captured_square = us == White ? move.to - 8 : move.to + 8;
        add_piece(undo.captured, captured_square);
    } else if (undo.captured != Empty) {
        add_piece(undo.captured, move.to);
    }

    key_ = undo.key;
}

void Position::make_null_move(UndoState& undo) {
    undo.captured = Empty;
    undo.castling_rights = castling_;
    undo.ep_square = ep_;
    undo.halfmove_clock = halfmove_;
    undo.fullmove_number = fullmove_;
    undo.key = key_;

    ep_ = NoSquare;
    ++halfmove_;
    if (stm_ == Black) {
        ++fullmove_;
    }
    stm_ = opposite(stm_);
    key_ = compute_key();
    push_history();
}

void Position::unmake_null_move(const UndoState& undo) {
    pop_history();
    stm_ = opposite(stm_);
    castling_ = undo.castling_rights;
    ep_ = undo.ep_square;
    halfmove_ = undo.halfmove_clock;
    fullmove_ = undo.fullmove_number;
    key_ = undo.key;
}

std::uint64_t Position::perft(int depth) {
    if (depth <= 0) {
        return 1;
    }

    std::vector<Move> moves;
    generate_legal_moves(moves);
    if (depth == 1) {
        return static_cast<std::uint64_t>(moves.size());
    }

    std::uint64_t total = 0;
    for (const Move& move : moves) {
        UndoState undo;
        if (!make_move(move, undo)) {
            continue;
        }
        total += perft(depth - 1);
        unmake_move(move, undo);
    }
    return total;
}

}  // namespace proton
