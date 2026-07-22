// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Pure partition of a prop batch by highlight state — no Qt, no GL — so it is
// unit-testable headless. The viewport draws all NON-highlighted instances of a
// batch as ONE instanced call (the fast path) and each highlighted instance as
// its own 1-instance draw so it can carry its own accent state.

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

#include "render/renderer.hpp"

namespace roadmaker::editor {

/// The split of a prop batch: the transforms of every non-highlighted instance
/// (drawn together) and the indices of the highlighted instances (drawn
/// individually). Indices address the batch's instance/transform arrays.
struct PropPartition {
  std::vector<InstanceData> kept;       ///< non-highlighted transforms
  std::vector<std::size_t> highlighted; ///< indices of highlighted instances
};

/// Partitions `transforms` by the index-parallel `states`: an instance whose
/// state is None goes into `kept`; anything else (Hover/Selected) is recorded in
/// `highlighted`. Extra entries beyond the shorter span are ignored.
[[nodiscard]] inline PropPartition partition_prop_batch(std::span<const HighlightState> states,
                                                        std::span<const InstanceData> transforms) {
  PropPartition out;
  const std::size_t count = std::min(states.size(), transforms.size());
  out.kept.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (states[i] == HighlightState::None) {
      out.kept.push_back(transforms[i]);
    } else {
      out.highlighted.push_back(i);
    }
  }
  return out;
}

} // namespace roadmaker::editor
