#include "uci.h"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <vector>

namespace proton {

namespace {

std::vector<std::string> split(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::string> parts;
    std::string part;
    while (stream >> part) {
        parts.push_back(part);
    }
    return parts;
}

Backend parse_backend(const std::string& value) {
    if (value == "gpu") {
        return Backend::Gpu;
    }
    if (value == "hybrid") {
        return Backend::Hybrid;
    }
    return Backend::Cpu;
}

}  // namespace

UciLoop::UciLoop() {
    apply_options();
}

void UciLoop::print_uci() const {
    std::cout << "id name Proton Chess\n";
    std::cout << "id author Codex\n";
    std::cout << "option name Backend type combo default cpu var cpu var gpu var hybrid\n";
    std::cout << "option name Threads type spin default 1 min 1 max 256\n";
    std::cout << "option name Hash type spin default 64 min 1 max 65536\n";
    std::cout << "option name SyzygyPath type string default\n";
    std::cout << "option name DeepEvalBudgetMs type spin default 5 min 0 max 1000\n";
    std::cout << "option name DeepEvalBatchSize type spin default 16 min 1 max 4096\n";
    std::cout << "uciok\n";
}

void UciLoop::apply_options() {
    evaluator_.set_options(options_);
    search_.set_options(options_);
}

void UciLoop::handle_position(const std::string& line) {
    auto tokens = split(line);
    if (tokens.size() < 2) {
        return;
    }

    std::size_t index = 1;
    if (tokens[index] == "startpos") {
        position_.set_startpos();
        ++index;
    } else if (tokens[index] == "fen") {
        ++index;
        std::string fen;
        for (int fields = 0; index < tokens.size() && fields < 6; ++fields, ++index) {
            if (!fen.empty()) {
                fen.push_back(' ');
            }
            fen += tokens[index];
        }
        position_.set_fen(fen);
    }

    if (index < tokens.size() && tokens[index] == "moves") {
        ++index;
        for (; index < tokens.size(); ++index) {
            const Move move = position_.parse_uci_move(tokens[index]);
            if (move.is_null()) {
                break;
            }
            UndoState undo;
            position_.make_move(move, undo);
        }
    }
}

void UciLoop::handle_go(const std::string& line) {
    SearchLimits limits;
    auto tokens = split(line);
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        const std::string& token = tokens[i];
        auto next_int = [&](int& out) {
            if (i + 1 < tokens.size()) {
                out = std::stoi(tokens[++i]);
            }
        };
        if (token == "depth") next_int(limits.depth);
        else if (token == "movetime") next_int(limits.movetime_ms);
        else if (token == "wtime") next_int(limits.wtime_ms);
        else if (token == "btime") next_int(limits.btime_ms);
        else if (token == "winc") next_int(limits.winc_ms);
        else if (token == "binc") next_int(limits.binc_ms);
        else if (token == "infinite") limits.infinite = true;
    }

    const SearchResult result = search_.find_best_move(position_, limits, evaluator_);
    std::cout << "info depth " << result.depth << " score cp " << result.score_cp << " nodes " << result.nodes << "\n";
    std::cout << "bestmove " << result.best_move.to_uci() << "\n";
}

void UciLoop::handle_setoption(const std::string& line) {
    auto tokens = split(line);
    if (tokens.size() < 5) {
        return;
    }

    std::string name;
    std::string value;
    bool parsing_name = false;
    bool parsing_value = false;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "name") {
            parsing_name = true;
            parsing_value = false;
            continue;
        }
        if (tokens[i] == "value") {
            parsing_name = false;
            parsing_value = true;
            continue;
        }
        if (parsing_name) {
            if (!name.empty()) name.push_back(' ');
            name += tokens[i];
        } else if (parsing_value) {
            if (!value.empty()) value.push_back(' ');
            value += tokens[i];
        }
    }

    if (name == "Backend") options_.backend = parse_backend(value);
    else if (name == "Threads") options_.threads = std::max(1, std::stoi(value));
    else if (name == "Hash") options_.hash_mb = std::max(1, std::stoi(value));
    else if (name == "SyzygyPath") options_.syzygy_path = value;
    else if (name == "DeepEvalBudgetMs") options_.deep_eval_budget_ms = std::max(0, std::stoi(value));
    else if (name == "DeepEvalBatchSize") options_.deep_eval_batch_size = std::max(1, std::stoi(value));

    apply_options();
}

void UciLoop::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "uci") {
            print_uci();
        } else if (line == "isready") {
            std::cout << "readyok\n";
        } else if (line == "ucinewgame") {
            position_.set_startpos();
        } else if (line.rfind("position", 0) == 0) {
            handle_position(line);
        } else if (line.rfind("go", 0) == 0) {
            handle_go(line);
        } else if (line.rfind("setoption", 0) == 0) {
            handle_setoption(line);
        } else if (line.rfind("perft", 0) == 0) {
            const auto tokens = split(line);
            const int depth = tokens.size() > 1 ? std::stoi(tokens[1]) : 1;
            std::cout << "perft " << position_.perft(depth) << "\n";
        } else if (line == "moves") {
            std::vector<Move> moves;
            position_.generate_legal_moves(moves);
            for (const Move& move : moves) {
                std::cout << move.to_uci() << "\n";
            }
        } else if (line == "d") {
            std::cout << position_.fen() << "\n";
        } else if (line == "quit") {
            break;
        }
    }
}

}  // namespace proton
