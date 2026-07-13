#pragma once

// Behavior matrix for a library-item drop: maps a dropped catalogue item plus
// the drop location to an action the MainWindow carries out — a road template
// arms Create Road (the caller places the first waypoint), a T/X assembly
// produces a standalone-intersection command at the drop point. Pure logic
// (no widgets), so the mapping is unit-testable headless.
//
// Not yet: dropping a T on an existing road tees INTO it (splits it); v1
// places a standalone assembly at the drop point (follow-up).

#include "roadmaker/edit/command.hpp"
#include "roadmaker/road/authoring.hpp"

#include <QString>
#include <memory>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

enum class LibraryDropKind { None, RoadTemplate, Assembly };

struct LibraryDropAction {
  LibraryDropKind kind = LibraryDropKind::None;
  LaneProfile profile;                    ///< RoadTemplate: arm Create Road with this
  std::unique_ptr<edit::Command> command; ///< Assembly: push this (one undo unit)
  QString toast;                          ///< success message for the toast overlay
};

/// The LaneProfile for a road-template profile name (two_lane_rural default).
[[nodiscard]] LaneProfile profile_for(const QString& name);

/// Resolves a dropped `item` at (world_x, world_y) to a LibraryDropAction.
/// Unknown items / create kinds yield LibraryDropKind::None.
[[nodiscard]] LibraryDropAction resolve_library_drop(const LibraryItem& item,
                                                     const RoadNetwork& network,
                                                     double world_x,
                                                     double world_y);

} // namespace roadmaker::editor
