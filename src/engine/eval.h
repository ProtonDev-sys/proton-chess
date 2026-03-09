#pragma once

#include <string>
#include <vector>

#include "position.h"

namespace proton {

enum class Backend {
    Cpu,
    Gpu,
    Hybrid
};

struct EngineOptions {
    Backend backend = Backend::Cpu;
    int threads = 1;
    int hash_mb = 64;
    std::string syzygy_path;
    int deep_eval_budget_ms = 5;
    int deep_eval_batch_size = 16;
};

class CoreEvalNet {
public:
    [[nodiscard]] int evaluate(const Position& position) const;
};

class DeepEvalNet {
public:
    explicit DeepEvalNet(Backend backend = Backend::Cpu);

    [[nodiscard]] bool available() const;
    [[nodiscard]] int evaluate(const Position& position) const;
    [[nodiscard]] std::vector<float> score_moves(const Position& position, const std::vector<Move>& moves) const;

private:
    Backend backend_;
};

class Evaluator {
public:
    Evaluator() = default;

    void set_options(const EngineOptions& options);
    [[nodiscard]] int evaluate(const Position& position, bool deep_hint = false) const;
    [[nodiscard]] std::vector<float> policy_scores(const Position& position, const std::vector<Move>& moves) const;
    [[nodiscard]] bool deep_available() const;

private:
    EngineOptions options_{};
    CoreEvalNet core_{};
    DeepEvalNet deep_{Backend::Cpu};
};

}  // namespace proton
