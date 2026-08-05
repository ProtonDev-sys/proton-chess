#pragma once

#include "position.h"

namespace proton {

[[nodiscard]] int static_exchange_eval(const Position& position, const Move& move);
[[nodiscard]] inline bool static_exchange_ge(const Position& position,
                                             const Move& move,
                                             int threshold) {
    return static_exchange_eval(position, move) >= threshold;
}

}  // namespace proton
