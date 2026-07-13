#include "document/library_drop.hpp"

#include "roadmaker/edit/assembly.hpp"

namespace roadmaker::editor {

LaneProfile profile_for(const QString& name) {
  if (name == QStringLiteral("urban_sidewalk")) {
    return LaneProfile::urban_sidewalk();
  }
  if (name == QStringLiteral("highway")) {
    return LaneProfile::highway();
  }
  return LaneProfile::two_lane_rural();
}

LibraryDropAction resolve_library_drop(const LibraryItem& item,
                                       const RoadNetwork& network,
                                       double world_x,
                                       double world_y) {
  LibraryDropAction action;
  switch (item.kind) {
  case LibraryItem::Kind::RoadTemplate:
    action.kind = LibraryDropKind::RoadTemplate;
    action.profile = profile_for(item.profile);
    return action;
  case LibraryItem::Kind::Assembly: {
    const edit::assembly::Pose pose{.x = world_x, .y = world_y, .heading = 0.0};
    if (item.assembly == QStringLiteral("t")) {
      action.command = edit::assembly::t_intersection(network, pose);
      action.toast = QStringLiteral("Placed T-intersection — Ctrl+Z to undo");
    } else if (item.assembly == QStringLiteral("x")) {
      action.command = edit::assembly::x_intersection(network, pose);
      action.toast = QStringLiteral("Placed X-intersection — Ctrl+Z to undo");
    }
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Assembly;
    }
    return action;
  }
  case LibraryItem::Kind::Unknown:
    break;
  }
  return action;
}

} // namespace roadmaker::editor
