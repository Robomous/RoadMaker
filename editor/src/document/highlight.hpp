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

/// Feedback state for the mesh of `road`/`lane`/`object`/`junction` — a lane
/// patch (`lane` valid), a junction floor (`junction` valid, road invalid), a
/// road-level mesh such as a marking (all invalid but `road`), or a placed prop
/// part (`object` valid). A prop matches only by object id (selecting a road
/// never lights its trees, and vice versa); a junction floor matches only by
/// junction id; a lane-level target matches by lane id; a road-level target
/// matches by road id but never a prop or floor. Selected beats Hover.
[[nodiscard]] inline HighlightState highlight_state_for(RoadId road,
                                                        LaneId lane,
                                                        ObjectId object,
                                                        JunctionId junction,
                                                        std::span<const SelectionEntry> selection,
                                                        RoadId hovered_road,
                                                        LaneId hovered_lane,
                                                        ObjectId hovered_object,
                                                        JunctionId hovered_junction) {
  const auto matches = [&](RoadId r, LaneId l, ObjectId o, JunctionId j) {
    if (j.is_valid()) {
      return junction.is_valid() && junction == j;
    }
    if (o.is_valid()) {
      return object.is_valid() && object == o;
    }
    if (l.is_valid()) {
      return lane == l;
    }
    return !object.is_valid() && !junction.is_valid() && r.is_valid() && road == r;
  };
  for (const SelectionEntry& entry : selection) {
    if (matches(entry.road, entry.lane, entry.object, entry.junction)) {
      return HighlightState::Selected;
    }
  }
  if (matches(hovered_road, hovered_lane, hovered_object, hovered_junction)) {
    return HighlightState::Hover;
  }
  return HighlightState::None;
}

} // namespace roadmaker::editor
