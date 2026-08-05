#pragma once

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "search.h"

namespace proton {

class UciLoop {
public:
    UciLoop();
    ~UciLoop();

    void run();

private:
    Position position_{};
    EngineOptions options_{};
    Evaluator evaluator_{};
    Search search_;
    std::thread worker_{};
    std::mutex output_mutex_{};
    std::mutex ponder_mutex_{};
    std::condition_variable ponder_cv_{};
    bool ponder_waiting_ = false;

    void handle_command(const std::string& line, bool& quit);
    void handle_position(const std::string& line);
    void handle_setoption(const std::string& line);
    void handle_go(const std::string& line);
    void start_search(const SearchLimits& limits);
    void stop_search();
    void handle_ponder_hit();
    void print_info(const SearchInfo& info);
    void print_line(const std::string& text);

    [[nodiscard]] static std::string lowercase(std::string text);
    [[nodiscard]] static int mate_distance(int score);
};

}  // namespace proton
