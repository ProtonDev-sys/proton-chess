#pragma once

#include <vector>

#include "position.h"

namespace proton {

// Learned human-club-v1 move-frequency logits in the supplied legal-move order.
// Values retain the model's raw units: no softmax, clipping, search, or randomness.
// The position is preserved; candidates must be legal moves for this position.
[[nodiscard]] std::vector<float> human_move_logits(
    const Position& position, const std::vector<Move>& moves);

}  // namespace proton
