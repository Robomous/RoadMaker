#pragma once

// Behavior matrix for a library-item drop: maps a dropped catalogue item plus
// the drop location to an action the MainWindow carries out — a road template
// arms Create Road (the caller places the first waypoint); a T/X assembly
// dropped ON a road tees/crosses INTO it (aligned to the road tangent, split +
// junction), and a drop in open space places a standalone intersection at the
// cursor. Pure logic (no widgets), so the mapping is unit-testable headless.

#include "roadmaker/edit/command.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road_style.hpp"

#include <QString>
#include <memory>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

enum class LibraryDropKind { None, RoadTemplate, RoadStyle, Assembly, Tree, Signal, Marking };

/// Where a resolved drop lands in the world (x, y) and whether it is valid
/// there. The drag ghost renders at this point, so what the user sees while
/// dragging is exactly where the element commits (ghost==commit). An invalid
/// preview tints the ghost and the drop is rejected — never silently relocated.
struct PlacementPreview {
  double x = 0.0;
  double y = 0.0;
  bool valid = false;
};

struct LibraryDropAction {
  LibraryDropKind kind = LibraryDropKind::None;
  LaneProfile profile;                    ///< RoadTemplate: arm Create Road with this
  std::unique_ptr<edit::Command> command; ///< Assembly/Tree/RoadStyle: push this (one undo unit)
  RoadId target_road;                     ///< RoadStyle: the road under the cursor (for highlight)
  QString toast;                          ///< success message, or (kind None) a reject hint
  PlacementPreview preview;               ///< where the ghost/commit lands (ghost==commit)
};

/// The LaneProfile for a road-template profile name (two_lane_rural default).
[[nodiscard]] LaneProfile profile_for(const QString& name);

/// The RoadStyle for a road-style name (urban_two_lane default). Shared with the
/// Attributes-pane road-style slot so the drop and the slot agree.
[[nodiscard]] RoadStyle style_for(const QString& name);

/// Resolves a dropped `item` at (world_x, world_y) to a LibraryDropAction.
/// Unknown items / create kinds yield LibraryDropKind::None.
[[nodiscard]] LibraryDropAction resolve_library_drop(const LibraryItem& item,
                                                     const RoadNetwork& network,
                                                     double world_x,
                                                     double world_y);

} // namespace roadmaker::editor
