#include "document/library_drop.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <cmath>
#include <optional>
#include <set>
#include <string>

#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// A tree snaps to a road within this lateral distance [m] of its reference
/// line — generous enough to place a tree on the verge, tight enough that a
/// drop in open space is rejected (OpenDRIVE objects are road-relative).
constexpr double kTreeSnapThreshold = 12.0;

struct RoadStation {
  RoadId road;
  double s = 0.0;
  double t = 0.0;
};

/// The nearest road to (x, y) whose reference line passes within `max_t`, with
/// the drop's road-relative (s, t). nullopt when no road is close enough.
std::optional<RoadStation>
nearest_road_station(const RoadNetwork& network, double x, double y, double max_t) {
  std::optional<RoadStation> best;
  double best_abs_t = max_t;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (road.plan_view.empty()) {
      return;
    }
    const StationCoord station = find_station(road.plan_view, x, y);
    if (std::abs(station.t) < best_abs_t) {
      best_abs_t = std::abs(station.t);
      best = RoadStation{.road = id, .s = station.s, .t = station.t};
    }
  });
  return best;
}

/// Lowest positive integer odr id not already used by an object (id-unique).
std::string next_object_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_object([&](ObjectId, const Object& object) { taken.insert(object.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

} // namespace

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
  case LibraryItem::Kind::Tree: {
    // Props are road-relative: snap to the nearest road, or reject with a hint
    // (OpenDRIVE has no world-placed object).
    const auto placement = nearest_road_station(network, world_x, world_y, kTreeSnapThreshold);
    if (!placement.has_value()) {
      action.toast = QStringLiteral("Drop a tree onto or beside a road");
      return action; // kind stays None — the caller surfaces the hint
    }
    Object tree;
    tree.odr_id = next_object_odr_id(network);
    tree.name = item.model.toStdString();
    tree.type = item.model == QStringLiteral("shrub") ? ObjectType::Vegetation : ObjectType::Tree;
    tree.s = placement->s;
    tree.t = placement->t;
    if (const props::PropModel* model = props::model(tree.name)) {
      tree.radius = model->radius;
      tree.height = model->height;
    }
    action.command = edit::add_object(network, placement->road, std::move(tree));
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Tree;
      action.toast = QStringLiteral("Placed %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
  case LibraryItem::Kind::Unknown:
    break;
  }
  return action;
}

} // namespace roadmaker::editor
