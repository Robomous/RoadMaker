#pragma once

// Pure mapping from an entity (road + optional lane) to its viewport feedback
// state, given the current selection and the hovered entity. Selection wins
// over hover. Kept free of Qt/GL and any widget so it is unit-testable
// headless — the viewport is the only caller, but the rule lives here.

#include "roadmaker/road/id.hpp"

#include <span>

#include "document/selection_model.hpp"
#include "render/renderer.hpp"

namespace roadmaker::editor {

/// Feedback state for the mesh of `road`/`lane` (lane invalid = a road-level
/// mesh such as a marking or junction floor). An entry/hover matches a
/// lane-level target by lane id and a road-level target by road id — the same
/// rule the scene tree and picking use. Selected takes priority over Hover.
[[nodiscard]] inline HighlightState highlight_state_for(RoadId road,
                                                        LaneId lane,
                                                        std::span<const SelectionEntry> selection,
                                                        RoadId hovered_road,
                                                        LaneId hovered_lane) {
  const auto matches = [&](RoadId r, LaneId l) {
    return l.is_valid() ? lane == l : (r.is_valid() && road == r);
  };
  for (const SelectionEntry& entry : selection) {
    if (matches(entry.road, entry.lane)) {
      return HighlightState::Selected;
    }
  }
  if (matches(hovered_road, hovered_lane)) {
    return HighlightState::Hover;
  }
  return HighlightState::None;
}

} // namespace roadmaker::editor
