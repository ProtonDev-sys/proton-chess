#pragma once

#include <string>

#include "search.h"

namespace proton {

class UciLoop {
public:
    UciLoop();

    void run();

private:
    Position position_{};
    EngineOptions options_{};
    Evaluator evaluator_{};
    Search search_{};

    void print_uci() const;
    void handle_position(const std::string& line);
    void handle_go(const std::string& line);
    void handle_setoption(const std::string& line);
    void apply_options();
};

}  // namespace proton
