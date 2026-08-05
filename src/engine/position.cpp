#include "position.h"

#include "attacks.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <sstream>

namespace proton {
namespace {

constexpr int WhiteKingSide = 1 << 0;
constexpr int WhiteQueenSide = 1 << 1;
constexpr int BlackKingSide = 1 << 2;
constexpr int BlackQueenSide = 1 << 3;

int castling_mask_for_rook_square(int square) {
    switch (square) {
    case 0: return WhiteQueenSide;
    case 7: return WhiteKingSide;
    case 56: return BlackQueenSide;
    case 63: return BlackKingSide;
    default: return 0;
    }
}

std::uint64_t splitmix64(std::uint64_t& seed) {
    std::uint64_t z = (seed += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct ZobristTables {
    std::array<std::array<std::uint64_t, 64>, 13> piece{};
    std::array<std::uint64_t, 16> castling{};
    std::array<std::uint64_t, 8> ep{};
    std::uint64_t side = 0;
};

const ZobristTables& zobrist() {
    static const ZobristTables tables = [] {
        ZobristTables out;
        std::uint64_t seed = 0x50524f544f4e4348ULL;  // "PROTONCH"
        for (auto& piece_table : out.piece) {
            for (std::uint64_t& value : piece_table) value = splitmix64(seed);
        }
        for (std::uint64_t& value : out.castling) value = splitmix64(seed);
        for (std::uint64_t& value : out.ep) value = splitmix64(seed);
        out.side = splitmix64(seed);
        return out;
    }();
    return tables;
}

}  // namespace

Position::Position() {
    history_keys_.reserve(256);
    null_barriers_.reserve(8);
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
    pawn_key_ = 0;
    history_keys_.clear();
    null_barriers_.clear();
}

void Position::rebuild_bitboards() {
    pieces_.fill(0);
    occupancy_.fill(0);
    pawn_key_ = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        if (piece == Empty) continue;
        pieces_[piece] |= bit(square);
        const Color color = piece >= BlackPawn ? Black : White;
        occupancy_[color] |= bit(square);
        if (piece_type(piece) == Pawn) pawn_key_ ^= zobrist().piece[piece][square];
    }
}

void Position::add_piece(Piece piece, int square) {
    if (piece == Empty || piece > BlackKing || !attacks::on_board(square)) return;
    board_[square] = piece;
    pieces_[piece] |= bit(square);
    const Color color = piece >= BlackPawn ? Black : White;
    occupancy_[color] |= bit(square);
    if (piece_type(piece) == Pawn) pawn_key_ ^= zobrist().piece[piece][square];
}

void Position::remove_piece(Piece piece, int square) {
    if (piece == Empty || piece > BlackKing || !attacks::on_board(square)) return;
    board_[square] = Empty;
    pieces_[piece] &= ~bit(square);
    const Color color = piece >= BlackPawn ? Black : White;
    occupancy_[color] &= ~bit(square);
    if (piece_type(piece) == Pawn) pawn_key_ ^= zobrist().piece[piece][square];
}

void Position::move_piece(Piece piece, int from, int to) {
    remove_piece(piece, from);
    add_piece(piece, to);
}

int Position::ep_hash_file() const {
    if (ep_ == NoSquare) return -1;
    const Piece pawn = make_piece(stm_, Pawn);
    const Color them = opposite(stm_);
    const Piece enemy_pawn = make_piece(them, Pawn);
    const int captured_square = ep_ + (stm_ == White ? -8 : 8);
    if (!attacks::on_board(captured_square) || board_[ep_] != Empty ||
        board_[captured_square] != enemy_pawn) {
        return -1;
    }
    const int source_a = stm_ == White ? ep_ - 9 : ep_ + 7;
    const int source_b = stm_ == White ? ep_ - 7 : ep_ + 9;

    for (const int source : {source_a, source_b}) {
        if (!attacks::on_board(source) ||
            std::abs(file_of(source) - file_of(ep_)) != 1 || board_[source] != pawn) {
            continue;
        }

        std::array<Bitboard, 13> pieces = pieces_;
        pieces[pawn] &= ~bit(source);
        pieces[pawn] |= bit(ep_);
        pieces[enemy_pawn] &= ~bit(captured_square);

        Bitboard occupied = occupancy_all();
        occupied &= ~bit(source);
        occupied &= ~bit(captured_square);
        occupied |= bit(ep_);

        const int king = king_square(stm_);
        const Bitboard enemy_occupancy = occupancy_[them] & ~bit(captured_square);
        if (king != NoSquare &&
            (attacks::attackers_to(king, occupied, pieces) & enemy_occupancy) == 0) {
            return file_of(ep_);
        }
    }
    return -1;
}

std::uint64_t Position::compute_key() const {
    const auto& z = zobrist();
    std::uint64_t hash = 0;
    for (int square = 0; square < 64; ++square) {
        const Piece piece = board_[square];
        if (piece != Empty) hash ^= z.piece[piece][square];
    }
    hash ^= z.castling[castling_ & 15];
    const int ep_file = ep_hash_file();
    if (ep_file >= 0) hash ^= z.ep[ep_file];
    if (stm_ == Black) hash ^= z.side;
    return hash;
}

void Position::push_history() { history_keys_.push_back(key_); }

void Position::pop_history() {
    if (!history_keys_.empty()) history_keys_.pop_back();
}

void Position::reset_history() {
    history_keys_.clear();
    history_keys_.push_back(key_);
}

void Position::set_startpos() {
    set_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

bool Position::set_fen(const std::string& fen_text) {
    std::istringstream stream(fen_text);
    std::string board_part;
    std::string stm_part;
    std::string castling_part;
    std::string ep_part;
    if (!(stream >> board_part >> stm_part >> castling_part >> ep_part)) return false;
    int halfmove = 0;
    int fullmove = 1;
    if (!(stream >> halfmove)) halfmove = 0;
    if (!(stream >> fullmove)) fullmove = 1;
    if (halfmove < 0 || fullmove <= 0) return false;

    std::array<Piece, 64> board{};
    board.fill(Empty);

    int rank = 7;
    int file = 0;
    for (char c : board_part) {
        if (c == '/') {
            if (file != 8 || rank == 0) return false;
            --rank;
            file = 0;
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(c))) {
            const int empty = c - '0';
            if (empty < 1 || empty > 8 || file + empty > 8) return false;
            file += empty;
            continue;
        }
        const Piece piece = char_to_piece(c);
        if (piece == Empty || file >= 8 || rank < 0) return false;
        board[rank * 8 + file] = piece;
        ++file;
    }
    if (rank != 0 || file != 8) return false;

    Color stm = NoColor;
    if (stm_part == "w") stm = White;
    else if (stm_part == "b") stm = Black;
    else return false;

    int castling = 0;
    if (castling_part != "-") {
        for (char c : castling_part) {
            switch (c) {
            case 'K': castling |= WhiteKingSide; break;
            case 'Q': castling |= WhiteQueenSide; break;
            case 'k': castling |= BlackKingSide; break;
            case 'q': castling |= BlackQueenSide; break;
            default: return false;
            }
        }
    }

    const int ep = ep_part == "-" ? NoSquare : square_from_string(ep_part);
    if (ep != NoSquare && rank_of(ep) != 2 && rank_of(ep) != 5) return false;

    int white_kings = 0;
    int black_kings = 0;
    for (const Piece piece : board) {
        white_kings += piece == WhiteKing ? 1 : 0;
        black_kings += piece == BlackKing ? 1 : 0;
    }
    if (white_kings != 1 || black_kings != 1) return false;
    constexpr Bitboard BackRanks = 0xFF000000000000FFULL;
    Bitboard pawns = 0;
    for (int square = 0; square < 64; ++square) {
        if (piece_type(board[square]) == Pawn) pawns |= bit(square);
    }
    if ((pawns & BackRanks) != 0) return false;

    clear();
    board_ = board;
    stm_ = stm;
    castling_ = castling;
    ep_ = ep;
    halfmove_ = halfmove;
    fullmove_ = fullmove;
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
            if (empty != 0) {
                board_text.push_back(static_cast<char>('0' + empty));
                empty = 0;
            }
            board_text.push_back(piece_to_char(piece));
        }
        if (empty != 0) board_text.push_back(static_cast<char>('0' + empty));
        if (rank != 0) board_text.push_back('/');
    }

    std::string castling_text;
    if (castling_ & WhiteKingSide) castling_text.push_back('K');
    if (castling_ & WhiteQueenSide) castling_text.push_back('Q');
    if (castling_ & BlackKingSide) castling_text.push_back('k');
    if (castling_ & BlackQueenSide) castling_text.push_back('q');
    if (castling_text.empty()) castling_text = "-";

    return board_text + (stm_ == White ? " w " : " b ") + castling_text + " " +
           (ep_ == NoSquare ? "-" : square_to_string(ep_)) + " " +
           std::to_string(halfmove_) + " " + std::to_string(fullmove_);
}

int Position::king_square(Color color) const {
    const Bitboard kings = pieces_[color == White ? WhiteKing : BlackKing];
    return kings == 0 ? NoSquare : static_cast<int>(std::countr_zero(kings));
}

bool Position::is_square_attacked(int square, Color by) const {
    if (!attacks::on_board(square) || by == NoColor) return false;
    return (attacks::attackers_to(square, occupancy_all(), pieces_) & occupancy_[by]) != 0;
}

bool Position::in_check(Color color) const {
    const int square = king_square(color);
    return square != NoSquare && is_square_attacked(square, opposite(color));
}

bool Position::has_non_pawn_material(Color color) const {
    const Piece knight = color == White ? WhiteKnight : BlackKnight;
    const Piece bishop = color == White ? WhiteBishop : BlackBishop;
    const Piece rook = color == White ? WhiteRook : BlackRook;
    const Piece queen = color == White ? WhiteQueen : BlackQueen;
    return (pieces_[knight] | pieces_[bishop] | pieces_[rook] | pieces_[queen]) != 0;
}

bool Position::is_draw_by_material() const {
    if ((pieces_[WhitePawn] | pieces_[BlackPawn] | pieces_[WhiteRook] |
         pieces_[BlackRook] | pieces_[WhiteQueen] | pieces_[BlackQueen]) != 0) {
        return false;
    }

    const int knights = std::popcount(pieces_[WhiteKnight]) + std::popcount(pieces_[BlackKnight]);
    const int bishops = std::popcount(pieces_[WhiteBishop]) + std::popcount(pieces_[BlackBishop]);
    const int minors = knights + bishops;
    if (minors <= 1) return true;

    // Multiple bishops are dead material only when every bishop is confined to
    // the same square colour and no knight remains. K+N vs K+N and K+B vs K+N
    // are not dead positions: legal mating positions exist with help from the
    // defender's minor piece.
    if (knights == 0) {
        int bishop_colours = 0;
        Bitboard all_bishops = pieces_[WhiteBishop] | pieces_[BlackBishop];
        while (all_bishops != 0) {
            const int square = static_cast<int>(std::countr_zero(all_bishops));
            all_bishops &= all_bishops - 1;
            bishop_colours |= ((file_of(square) + rank_of(square)) & 1) ? 1 : 2;
        }
        return bishop_colours != 3;
    }
    return false;
}

bool Position::is_repetition(int required_occurrences) const {
    required_occurrences = std::max(2, required_occurrences);
    const std::size_t minimum_history = static_cast<std::size_t>(
        1 + 2 * (required_occurrences - 1));
    if (history_keys_.size() < minimum_history) return false;

    const int last = static_cast<int>(history_keys_.size()) - 1;
    int earliest = std::max(0, last - halfmove_);
    if (!null_barriers_.empty()) {
        earliest = std::max(earliest, static_cast<int>(null_barriers_.back()));
    }
    int occurrences = 1;  // Current position.
    for (int index = last - 2; index >= earliest; index -= 2) {
        if (history_keys_[static_cast<std::size_t>(index)] == key_ &&
            ++occurrences >= required_occurrences) {
            return true;
        }
    }
    return false;
}

void Position::generate_pseudo_legal_moves(std::vector<Move>& moves, bool captures_only) const {
    moves.clear();
    moves.reserve(64);

    const Color us = stm_;
    const Color them = opposite(us);
    for (int type_index = Pawn; type_index <= King; ++type_index) {
        const PieceType type = static_cast<PieceType>(type_index);
        const Piece piece = make_piece(us, type);
        Bitboard remaining = pieces_[piece];
        while (remaining != 0) {
            const int square = static_cast<int>(std::countr_zero(remaining));
            remaining &= remaining - 1;

            switch (type) {
            case Pawn: {
                const int direction = us == White ? 8 : -8;
                const int start_rank = us == White ? 1 : 6;
                const int promotion_rank = us == White ? 6 : 1;
                const int one_forward = square + direction;
                if (attacks::on_board(one_forward) && board_[one_forward] == Empty &&
                    (!captures_only || rank_of(square) == promotion_rank)) {
                    if (rank_of(square) == promotion_rank) {
                        for (PieceType promo : {Queen, Rook, Bishop, Knight}) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square),
                                                 static_cast<std::uint8_t>(one_forward), promo,
                                                 MovePromotion});
                        }
                    } else if (!captures_only) {
                        moves.push_back(Move{static_cast<std::uint8_t>(square),
                                             static_cast<std::uint8_t>(one_forward),
                                             NoPieceType, MoveQuiet});
                        const int two_forward = square + direction * 2;
                        if (rank_of(square) == start_rank && board_[two_forward] == Empty) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square),
                                                 static_cast<std::uint8_t>(two_forward),
                                                 NoPieceType, MoveDoublePush});
                        }
                    }
                }

                const std::array<int, 2> deltas = us == White
                    ? std::array<int, 2>{7, 9}
                    : std::array<int, 2>{-9, -7};
                for (int delta : deltas) {
                    const int target = square + delta;
                    if (!attacks::valid_step(square, target, delta)) continue;
                    if (target == ep_) {
                        const int captured_square = us == White ? target - 8 : target + 8;
                        if (board_[captured_square] == make_piece(them, Pawn)) {
                            moves.push_back(Move{static_cast<std::uint8_t>(square),
                                                 static_cast<std::uint8_t>(target), NoPieceType,
                                                 static_cast<std::uint8_t>(MoveCapture | MoveEnPassant)});
                        }
                        continue;
                    }
                    if (board_[target] != Empty && piece_color(board_[target]) == them) {
                        if (rank_of(square) == promotion_rank) {
                            for (PieceType promo : {Queen, Rook, Bishop, Knight}) {
                                moves.push_back(Move{static_cast<std::uint8_t>(square),
                                                     static_cast<std::uint8_t>(target), promo,
                                                     static_cast<std::uint8_t>(MoveCapture | MovePromotion)});
                            }
                        } else {
                            moves.push_back(Move{static_cast<std::uint8_t>(square),
                                                 static_cast<std::uint8_t>(target),
                                                 NoPieceType, MoveCapture});
                        }
                    }
                }
                break;
            }
            case Knight: {
                Bitboard targets = attacks::Knight[square] &
                    (captures_only ? occupancy_[them] : ~occupancy_[us]);
                while (targets != 0) {
                    const int target = static_cast<int>(std::countr_zero(targets));
                    targets &= targets - 1;
                    const std::uint8_t flags = (occupancy_[them] & bit(target)) != 0
                        ? MoveCapture : MoveQuiet;
                    moves.push_back(Move{static_cast<std::uint8_t>(square),
                                         static_cast<std::uint8_t>(target),
                                         NoPieceType, flags});
                }
                break;
            }
            case Bishop:
            case Rook:
            case Queen: {
                const Bitboard occupied = occupancy_all();
                Bitboard targets = type == Bishop ? attacks::bishop(square, occupied) :
                                   type == Rook ? attacks::rook(square, occupied) :
                                                  attacks::queen(square, occupied);
                targets &= captures_only ? occupancy_[them] : ~occupancy_[us];
                while (targets != 0) {
                    const int target = static_cast<int>(std::countr_zero(targets));
                    targets &= targets - 1;
                    const std::uint8_t flags = (occupancy_[them] & bit(target)) != 0
                        ? MoveCapture : MoveQuiet;
                    moves.push_back(Move{static_cast<std::uint8_t>(square),
                                         static_cast<std::uint8_t>(target),
                                         NoPieceType, flags});
                }
                break;
            }
            case King: {
                Bitboard targets = attacks::King[square] &
                    (captures_only ? occupancy_[them] : ~occupancy_[us]);
                while (targets != 0) {
                    const int target = static_cast<int>(std::countr_zero(targets));
                    targets &= targets - 1;
                    const std::uint8_t flags = (occupancy_[them] & bit(target)) != 0
                        ? MoveCapture : MoveQuiet;
                    moves.push_back(Move{static_cast<std::uint8_t>(square),
                                         static_cast<std::uint8_t>(target),
                                         NoPieceType, flags});
                }

                if (captures_only || in_check(us)) break;
                if (us == White && square == 4 && board_[4] == WhiteKing) {
                    if ((castling_ & WhiteKingSide) && board_[7] == WhiteRook &&
                        board_[5] == Empty && board_[6] == Empty &&
                        !is_square_attacked(5, them) && !is_square_attacked(6, them)) {
                        moves.push_back(Move{4, 6, NoPieceType, MoveKingCastle});
                    }
                    if ((castling_ & WhiteQueenSide) && board_[0] == WhiteRook &&
                        board_[1] == Empty && board_[2] == Empty && board_[3] == Empty &&
                        !is_square_attacked(3, them) && !is_square_attacked(2, them)) {
                        moves.push_back(Move{4, 2, NoPieceType, MoveQueenCastle});
                    }
                } else if (us == Black && square == 60 && board_[60] == BlackKing) {
                    if ((castling_ & BlackKingSide) && board_[63] == BlackRook &&
                        board_[61] == Empty && board_[62] == Empty &&
                        !is_square_attacked(61, them) && !is_square_attacked(62, them)) {
                        moves.push_back(Move{60, 62, NoPieceType, MoveKingCastle});
                    }
                    if ((castling_ & BlackQueenSide) && board_[56] == BlackRook &&
                        board_[57] == Empty && board_[58] == Empty && board_[59] == Empty &&
                        !is_square_attacked(59, them) && !is_square_attacked(58, them)) {
                        moves.push_back(Move{60, 58, NoPieceType, MoveQueenCastle});
                    }
                }
                break;
            }
            default: break;
            }
        }
    }
}

void Position::generate_pseudo_moves(std::vector<Move>& moves) const {
    generate_pseudo_legal_moves(moves, false);
}

void Position::generate_pseudo_captures(std::vector<Move>& moves) const {
    generate_pseudo_legal_moves(moves, true);
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

Move Position::parse_uci_move(const std::string& text) const {
    if (text.size() < 4 || text.size() > 5) return Move::null();
    std::vector<Move> legal;
    generate_legal_moves(legal);
    for (const Move& move : legal) {
        if (move.to_uci() == text) return move;
    }
    return Move::null();
}

bool Position::make_move_unchecked(const Move& move, UndoState& undo) {
    if (move.is_null() || move.from >= 64 || move.to >= 64) return false;

    const Color us = stm_;
    const Color them = opposite(us);
    const Piece moving = board_[move.from];
    if (moving == Empty || piece_color(moving) != us) return false;

    Piece captured = board_[move.to];
    int captured_square = move.to;
    if ((move.flags & MoveEnPassant) != 0) {
        captured_square = us == White ? move.to - 8 : move.to + 8;
        if (!attacks::on_board(captured_square)) return false;
        captured = board_[captured_square];
        if (captured != make_piece(them, Pawn) || board_[move.to] != Empty) return false;
    } else if (captured != Empty && piece_color(captured) != them) {
        return false;
    }

    if (move.is_promotion() &&
        (piece_type(moving) != Pawn || move.promotion < Knight || move.promotion > Queen)) {
        return false;
    }

    int rook_from = NoSquare;
    int rook_to = NoSquare;
    Piece castle_rook = Empty;
    if ((move.flags & MoveKingCastle) != 0) {
        rook_from = us == White ? 7 : 63;
        rook_to = us == White ? 5 : 61;
        castle_rook = make_piece(us, Rook);
    } else if ((move.flags & MoveQueenCastle) != 0) {
        rook_from = us == White ? 0 : 56;
        rook_to = us == White ? 3 : 59;
        castle_rook = make_piece(us, Rook);
    }
    if (rook_from != NoSquare && (piece_type(moving) != King || board_[rook_from] != castle_rook)) {
        return false;
    }

    undo.captured = captured;
    undo.captured_square = captured == Empty ? NoSquare : captured_square;
    undo.castling_rights = castling_;
    undo.ep_square = ep_;
    undo.halfmove_clock = halfmove_;
    undo.fullmove_number = fullmove_;
    undo.key = key_;

    const auto& z = zobrist();
    key_ ^= z.castling[castling_ & 15];
    const int old_ep_file = ep_hash_file();
    if (old_ep_file >= 0) key_ ^= z.ep[old_ep_file];

    key_ ^= z.piece[moving][move.from];
    if (captured != Empty) {
        key_ ^= z.piece[captured][captured_square];
        remove_piece(captured, captured_square);
    }
    remove_piece(moving, move.from);

    const Piece placed = move.is_promotion() ? make_piece(us, move.promotion) : moving;
    add_piece(placed, move.to);
    key_ ^= z.piece[placed][move.to];

    if (rook_from != NoSquare) {
        key_ ^= z.piece[castle_rook][rook_from];
        key_ ^= z.piece[castle_rook][rook_to];
        move_piece(castle_rook, rook_from, rook_to);
    }

    ep_ = NoSquare;
    if ((move.flags & MoveDoublePush) != 0) ep_ = us == White ? move.to - 8 : move.to + 8;

    if (piece_type(moving) == King) {
        castling_ &= us == White
            ? ~(WhiteKingSide | WhiteQueenSide)
            : ~(BlackKingSide | BlackQueenSide);
    }
    if (piece_type(moving) == Rook) castling_ &= ~castling_mask_for_rook_square(move.from);
    if (captured != Empty) castling_ &= ~castling_mask_for_rook_square(captured_square);

    halfmove_ = (piece_type(moving) == Pawn || captured != Empty) ? 0 : halfmove_ + 1;
    if (us == Black) ++fullmove_;
    stm_ = them;

    key_ ^= z.castling[castling_ & 15];
    const int new_ep_file = ep_hash_file();
    if (new_ep_file >= 0) key_ ^= z.ep[new_ep_file];
    key_ ^= z.side;

    push_history();
    return true;
}

bool Position::make_move(const Move& move, UndoState& undo) {
    const Color us = stm_;
    if (!make_move_unchecked(move, undo)) return false;
    if (in_check(us)) {
        unmake_move(move, undo);
        return false;
    }
    return true;
}

void Position::unmake_move(const Move& move, const UndoState& undo) {
    pop_history();

    stm_ = opposite(stm_);
    castling_ = undo.castling_rights;
    ep_ = undo.ep_square;
    halfmove_ = undo.halfmove_clock;
    fullmove_ = undo.fullmove_number;

    const Color us = stm_;
    Piece moved = board_[move.to];
    remove_piece(moved, move.to);
    if (move.is_promotion()) moved = make_piece(us, Pawn);
    add_piece(moved, move.from);

    if ((move.flags & MoveKingCastle) != 0) {
        const Piece rook = make_piece(us, Rook);
        move_piece(rook, us == White ? 5 : 61, us == White ? 7 : 63);
    } else if ((move.flags & MoveQueenCastle) != 0) {
        const Piece rook = make_piece(us, Rook);
        move_piece(rook, us == White ? 3 : 59, us == White ? 0 : 56);
    }

    if (undo.captured != Empty && undo.captured_square != NoSquare) {
        add_piece(undo.captured, undo.captured_square);
    }
    key_ = undo.key;
}

void Position::make_null_move(UndoState& undo) {
    undo.captured = Empty;
    undo.captured_square = NoSquare;
    undo.castling_rights = castling_;
    undo.ep_square = ep_;
    undo.halfmove_clock = halfmove_;
    undo.fullmove_number = fullmove_;
    undo.key = key_;

    const auto& z = zobrist();
    const int old_ep_file = ep_hash_file();
    if (old_ep_file >= 0) key_ ^= z.ep[old_ep_file];
    ep_ = NoSquare;
    stm_ = opposite(stm_);
    key_ ^= z.side;
    null_barriers_.push_back(history_keys_.size());
}

void Position::unmake_null_move(const UndoState& undo) {
    stm_ = opposite(stm_);
    castling_ = undo.castling_rights;
    ep_ = undo.ep_square;
    halfmove_ = undo.halfmove_clock;
    fullmove_ = undo.fullmove_number;
    key_ = undo.key;
    if (!null_barriers_.empty()) null_barriers_.pop_back();
}

std::uint64_t Position::perft(int depth) {
    if (depth <= 0) return 1;

    std::vector<Move> moves;
    generate_pseudo_moves(moves);
    std::uint64_t total = 0;
    for (const Move& move : moves) {
        UndoState undo;
        if (!make_move(move, undo)) continue;
        total += depth == 1 ? 1 : perft(depth - 1);
        unmake_move(move, undo);
    }
    return total;
}

}  // namespace proton
