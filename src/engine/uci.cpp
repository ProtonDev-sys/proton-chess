#include "uci.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace proton {
namespace {

bool parse_int(const std::string& text, int& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

bool parse_u64(const std::string& text, std::uint64_t& value) {
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end;
}

std::string join_tokens(const std::vector<std::string>& tokens,
                        std::size_t begin, std::size_t end) {
    std::string output;
    for (std::size_t index = begin; index < end; ++index) {
        if (!output.empty()) output.push_back(' ');
        output += tokens[index];
    }
    return output;
}

}  // namespace

UciLoop::UciLoop() : search_(evaluator_) {
    evaluator_.set_options(options_);
    search_.set_options(options_);
}

UciLoop::~UciLoop() { stop_search(); }

std::string UciLoop::lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return text;
}

void UciLoop::print_line(const std::string& text) {
    std::lock_guard lock(output_mutex_);
    std::cout << text << '\n' << std::flush;
}

int UciLoop::mate_distance(int score) {
    const int plies = Search::mate_score() - std::abs(score);
    const int moves = std::max(1, (plies + 1) / 2);
    return score >= 0 ? moves : -moves;
}

void UciLoop::print_info(const SearchInfo& info) {
    std::ostringstream output;
    output << "info depth " << info.depth
           << " seldepth " << info.selective_depth;
    if (std::abs(info.score_cp) >= Search::mate_threshold()) {
        output << " score mate " << mate_distance(info.score_cp);
    } else {
        output << " score cp " << info.score_cp;
    }
    output << " nodes " << info.nodes
           << " nps " << info.nps
           << " hashfull " << info.hashfull_permille
           << " time " << info.time_ms;
    if (!info.pv.empty()) {
        output << " pv";
        for (const Move& move : info.pv) output << ' ' << move.to_uci();
    }
    print_line(output.str());
}

void UciLoop::stop_search() {
    search_.request_stop();
    {
        std::lock_guard lock(ponder_mutex_);
        ponder_waiting_ = false;
    }
    ponder_cv_.notify_all();
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) worker_.join();
}

void UciLoop::handle_ponder_hit() {
    search_.ponder_hit();
    {
        std::lock_guard lock(ponder_mutex_);
        ponder_waiting_ = false;
    }
    ponder_cv_.notify_all();
}

void UciLoop::start_search(const SearchLimits& limits) {
    stop_search();
    {
        std::lock_guard lock(ponder_mutex_);
        ponder_waiting_ = limits.ponder;
    }
    if (limits.ponder) search_.begin_ponder();

    Position root = position_;
    worker_ = std::thread([this, root = std::move(root), limits]() mutable {
        const SearchResult result = search_.think(std::move(root), limits,
            [this](const SearchInfo& info) { print_info(info); });

        if (limits.ponder) {
            std::unique_lock lock(ponder_mutex_);
            ponder_cv_.wait(lock, [this] { return !ponder_waiting_; });
        }

        std::ostringstream output;
        output << "bestmove " << result.best.to_uci();
        if (!result.ponder.is_null()) output << " ponder " << result.ponder.to_uci();
        print_line(output.str());
    });
}

void UciLoop::handle_position(const std::string& line) {
    stop_search();
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(token);
    if (tokens.size() < 2) return;

    Position candidate;
    std::size_t index = 1;
    bool valid = false;
    if (tokens[index] == "startpos") {
        candidate.set_startpos();
        valid = true;
        ++index;
    } else if (tokens[index] == "fen") {
        ++index;
        const auto moves_it = std::find(tokens.begin() + static_cast<std::ptrdiff_t>(index),
                                        tokens.end(), "moves");
        const std::size_t fen_end = static_cast<std::size_t>(moves_it - tokens.begin());
        const std::string fen = join_tokens(tokens, index, fen_end);
        valid = candidate.set_fen(fen);
        index = fen_end;
    }

    if (!valid) {
        print_line("info string invalid position command");
        return;
    }

    if (index < tokens.size() && tokens[index] == "moves") ++index;
    for (; index < tokens.size(); ++index) {
        const Move move = candidate.parse_uci_move(tokens[index]);
        if (move.is_null()) {
            print_line("info string illegal move in position command: " + tokens[index]);
            return;
        }
        UndoState undo;
        if (!candidate.make_move(move, undo)) {
            print_line("info string failed to apply move: " + tokens[index]);
            return;
        }
    }
    position_ = std::move(candidate);
}

void UciLoop::handle_setoption(const std::string& line) {
    stop_search();

    const std::string marker = "setoption name ";
    if (line.rfind(marker, 0) != 0) return;
    const std::size_t value_pos = line.find(" value ", marker.size());
    const std::string name = line.substr(marker.size(),
        value_pos == std::string::npos ? std::string::npos : value_pos - marker.size());
    const std::string value = value_pos == std::string::npos ? "" : line.substr(value_pos + 7);
    const std::string key = lowercase(name);
    const std::string lower_value = lowercase(value);

    int number = 0;
    bool changed = true;
    if (key == "hash" && parse_int(value, number)) {
        options_.hash_mb = std::clamp(number, 1, 4096);
    } else if (key == "clear hash") {
        search_.new_game();
    } else if (key == "threads" && parse_int(value, number)) {
        options_.threads = 1;
        if (number != 1) print_line("info string this build is single-threaded; Threads remains 1");
    } else if (key == "backend") {
        options_.backend = Backend::Cpu;
        if (lower_value != "cpu") {
            print_line("info string GPU/hybrid evaluation is not implemented; using cpu");
        }
    } else if (key == "syzygypath") {
        options_.syzygy_path = value;
        print_line("info string Syzygy probing is not implemented in this native build");
    } else if (key == "deepevalbudgetms" && parse_int(value, number)) {
        options_.deep_eval_budget_ms = std::clamp(number, 0, 1000);
        print_line("info string DeepEvalBudgetMs retained for compatibility but inactive");
    } else if (key == "deepevalbatchsize" && parse_int(value, number)) {
        options_.deep_eval_batch_size = std::clamp(number, 1, 1024);
        print_line("info string DeepEvalBatchSize retained for compatibility but inactive");
    } else if (key == "usebook") {
        options_.use_book = lower_value == "true" || lower_value == "1";
    } else if (key == "bookfile") {
        options_.book_file = value;
    } else if (key == "bookrandomness" && parse_int(value, number)) {
        options_.book_randomness = std::clamp(number, 0, 100);
    } else if (key == "uci_limitstrength") {
        options_.human_style = lower_value == "true" || lower_value == "1";
    } else if (key == "uci_elo" && parse_int(value, number)) {
        const int elo = std::clamp(number, 800, 2800);
        options_.human_skill = std::clamp((elo - 800) / 100, 0, 20);
        options_.human_max_loss_cp = std::clamp((2800 - elo) / 8, 8, 250);
    } else if (key == "skill level" && parse_int(value, number)) {
        options_.human_skill = std::clamp(number, 0, 20);
        options_.human_style = options_.human_skill < 20;
    } else if (key == "humanstyle") {
        options_.human_style = lower_value == "true" || lower_value == "1";
    } else if (key == "humanskill" && parse_int(value, number)) {
        options_.human_skill = std::clamp(number, 0, 20);
        options_.human_style = true;
    } else if (key == "humanmaxlosscp" && parse_int(value, number)) {
        options_.human_max_loss_cp = std::clamp(number, 0, 500);
    } else if (key == "moveoverhead" && parse_int(value, number)) {
        options_.move_overhead_ms = std::clamp(number, 0, 5000);
    } else if (key == "contempt" && parse_int(value, number)) {
        options_.contempt_cp = std::clamp(number, -100, 100);
    } else if (key == "humanseed") {
        std::uint64_t seed = 0;
        if (parse_u64(value, seed)) options_.human_seed = seed;
    } else {
        changed = false;
        print_line("info string unknown or invalid option: " + name);
    }

    if (changed) {
        evaluator_.set_options(options_);
        search_.set_options(options_);
    }
}

void UciLoop::handle_go(const std::string& line) {
    SearchLimits limits;
    std::istringstream stream(line);
    std::vector<std::string> tokens;
    for (std::string token; stream >> token;) tokens.push_back(token);

    bool any_limit = false;
    const auto is_go_keyword = [](const std::string& token) {
        return token == "searchmoves" || token == "ponder" || token == "wtime" ||
               token == "btime" || token == "winc" || token == "binc" ||
               token == "movestogo" || token == "depth" || token == "nodes" ||
               token == "mate" || token == "movetime" || token == "infinite";
    };

    for (std::size_t index = 1; index < tokens.size(); ++index) {
        const std::string& token = tokens[index];
        auto next_int = [&](int& target) {
            if (index + 1 < tokens.size()) {
                int parsed = 0;
                if (parse_int(tokens[++index], parsed)) target = std::max(0, parsed);
            }
        };
        if (token == "searchmoves") {
            limits.search_moves_specified = true;
            while (index + 1 < tokens.size() && !is_go_keyword(tokens[index + 1])) {
                const Move move = position_.parse_uci_move(tokens[++index]);
                if (!move.is_null()) limits.search_moves.push_back(move);
            }
        } else if (token == "depth") {
            next_int(limits.depth);
            any_limit = true;
        } else if (token == "movetime") {
            next_int(limits.movetime_ms);
            any_limit = true;
        } else if (token == "wtime") {
            next_int(limits.white_time_ms);
            any_limit = true;
        } else if (token == "btime") {
            next_int(limits.black_time_ms);
            any_limit = true;
        } else if (token == "winc") next_int(limits.white_increment_ms);
        else if (token == "binc") next_int(limits.black_increment_ms);
        else if (token == "movestogo") next_int(limits.moves_to_go);
        else if (token == "nodes" && index + 1 < tokens.size()) {
            std::uint64_t parsed = 0;
            if (parse_u64(tokens[++index], parsed)) limits.node_limit = parsed;
            any_limit = true;
        } else if (token == "mate") {
            int moves = 0;
            next_int(moves);
            if (moves > 0) limits.depth = std::min(126, moves * 2);
            any_limit = true;
        } else if (token == "infinite") {
            limits.infinite = true;
            any_limit = true;
        } else if (token == "ponder") {
            limits.ponder = true;
            any_limit = true;
        }
    }
    if (!any_limit) limits.infinite = true;
    start_search(limits);
}

void UciLoop::handle_command(const std::string& line, bool& quit) {
    if (line == "uci") {
        print_line("id name Proton Chess");
        print_line("id author ProtonDev-sys");
        print_line("option name Hash type spin default 64 min 1 max 4096");
        print_line("option name Threads type spin default 1 min 1 max 1");
        print_line("option name Clear Hash type button");
        print_line("option name UseBook type check default true");
        print_line("option name BookFile type string default openings/book_lines.txt");
        print_line("option name BookRandomness type spin default 0 min 0 max 100");
        print_line("option name UCI_LimitStrength type check default false");
        print_line("option name UCI_Elo type spin default 2800 min 800 max 2800");
        print_line("option name Skill Level type spin default 20 min 0 max 20");
        print_line("option name HumanStyle type check default false");
        print_line("option name HumanSkill type spin default 20 min 0 max 20");
        print_line("option name HumanMaxLossCp type spin default 12 min 0 max 500");
        print_line("option name HumanSeed type string default 0");
        print_line("option name MoveOverhead type spin default 25 min 0 max 5000");
        print_line("option name Contempt type spin default 0 min -100 max 100");
        print_line("uciok");
    } else if (line == "isready") {
        print_line("readyok");
    } else if (line == "ucinewgame") {
        stop_search();
        position_.set_startpos();
        search_.new_game();
    } else if (line.rfind("setoption", 0) == 0) {
        handle_setoption(line);
    } else if (line.rfind("position", 0) == 0) {
        handle_position(line);
    } else if (line.rfind("go", 0) == 0) {
        handle_go(line);
    } else if (line == "stop") {
        stop_search();
    } else if (line == "ponderhit") {
        handle_ponder_hit();
    } else if (line == "d") {
        print_line("info string fen " + position_.fen());
    } else if (line == "eval") {
        stop_search();
        print_line("info string evaluation " + std::to_string(evaluator_.evaluate(position_)) + " cp");
    } else if (line.rfind("perft ", 0) == 0) {
        stop_search();
        int depth = 0;
        if (parse_int(line.substr(6), depth) && depth >= 0 && depth <= 10) {
            Position copy = position_;
            const auto start = std::chrono::steady_clock::now();
            const std::uint64_t nodes = copy.perft(depth);
            const auto time = std::max<std::int64_t>(1,
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count());
            print_line("info string perft depth " + std::to_string(depth) +
                       " nodes " + std::to_string(nodes) +
                       " time " + std::to_string(time) +
                       " nps " + std::to_string(nodes * 1000ULL /
                                                  static_cast<std::uint64_t>(time)));
        }
    } else if (line == "quit") {
        stop_search();
        quit = true;
    }
}

void UciLoop::run() {
    bool quit = false;
    for (std::string line; !quit && std::getline(std::cin, line);) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (!line.empty()) handle_command(line, quit);
    }
    stop_search();
}

}  // namespace proton
