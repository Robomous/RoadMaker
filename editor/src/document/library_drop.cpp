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

/// A T/X assembly dropped within this lateral distance [m] of a road's
/// reference line tees/crosses INTO that road (aligned); a drop farther out is
/// a standalone intersection at the cursor.
constexpr double kAssemblySnapThreshold = 8.0;

/// The drop must sit at least this far [m] from a road end to leave room for
/// the junction area; closer than this, the on-road attach would refuse, so the
/// drop falls back to a standalone assembly. Comfortably above the auto gap for
/// typical profiles/turn radii.
constexpr double kAssemblyEndMargin = 15.0;

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

/// A signal snaps to a road within this lateral distance [m] of its reference
/// line — same rationale as the tree threshold (OpenDRIVE signals are
/// road-relative; a drop in open space is rejected).
constexpr double kSignalSnapThreshold = 12.0;

/// Lowest positive integer odr id not already used by a signal (id-unique per
/// <signals>). Signals and objects have separate id spaces in OpenDRIVE.
std::string next_signal_odr_id(const RoadNetwork& network) {
  std::set<std::string> taken;
  network.for_each_signal([&](SignalId, const Signal& signal) { taken.insert(signal.odr_id); });
  int candidate = 1;
  while (taken.contains(std::to_string(candidate))) {
    ++candidate;
  }
  return std::to_string(candidate);
}

/// The Signal a "light"/"sign" library item authors at (s, t). A traffic light
/// is dynamic with the OpenDRIVE catalog type; a static sign defaults to a
/// German speed-limit-50 plate (type 274/50) — a recognisable regulatory sign
/// the user can retype in the properties panel. Orientation "+" faces the
/// signal along increasing s (the reference-line direction); the mesh builder
/// resolves the world heading from the road tangent.
Signal make_dropped_signal(const RoadNetwork& network, bool light, double s, double t) {
  Signal signal;
  signal.odr_id = next_signal_odr_id(network);
  signal.dynamic = light;
  signal.orientation = ObjectOrientation::Plus;
  signal.s = s;
  signal.t = t;
  if (light) {
    signal.type = "1000001"; // OpenDRIVE traffic-light catalog type
    signal.subtype = "-1";
    signal.country = "OpenDRIVE";
  } else {
    signal.type = "274"; // German regulatory speed-limit sign
    signal.subtype = "50";
    signal.country = "DE";
  }
  return signal;
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
  // The ghost renders at action.preview; default it to the raw cursor so an
  // unresolved/rejected drop tints there rather than jumping to the origin.
  action.preview = {world_x, world_y, false};
  switch (item.kind) {
  case LibraryItem::Kind::RoadTemplate:
    action.kind = LibraryDropKind::RoadTemplate;
    action.profile = profile_for(item.profile);
    action.preview.valid = true; // armed at the cursor
    return action;
  case LibraryItem::Kind::Assembly: {
    const bool is_t = item.assembly == QStringLiteral("t");
    const bool is_x = item.assembly == QStringLiteral("x");
    if (!is_t && !is_x) {
      return action; // unknown assembly (preview invalid at cursor)
    }
    // Dropped ON a road (finding 1): project onto it and tee/cross INTO it,
    // aligned to the road tangent, instead of a floating standalone at the
    // cursor. A drop near a road END (no room for the junction area) or in open
    // space falls back to a standalone assembly, with a toast noting it.
    const auto on_road = nearest_road_station(network, world_x, world_y, kAssemblySnapThreshold);
    if (on_road.has_value()) {
      const Road* road = network.road(on_road->road);
      if (road != nullptr && on_road->s > kAssemblyEndMargin &&
          on_road->s < road->plan_view.length() - kAssemblyEndMargin) {
        action.command = is_t ? edit::assembly::tee_onto_road(network, on_road->road, on_road->s)
                              : edit::assembly::cross_onto_road(network, on_road->road, on_road->s);
        if (action.command != nullptr) {
          action.kind = LibraryDropKind::Assembly;
          // Ghost sits on the road at the tee/cross station (t = 0), where the
          // junction forms — not at the off-to-the-side cursor.
          const auto p = station_to_world(road->plan_view, on_road->s, 0.0);
          action.preview = {p[0], p[1], true};
          action.toast =
              is_t ? QStringLiteral("Teed a T-intersection into the road — Ctrl+Z to undo")
                   : QStringLiteral("Crossed an X-intersection over the road — Ctrl+Z to "
                                    "undo");
        }
        return action;
      }
    }
    // Off-road (or too near an end): a standalone assembly at the cursor.
    const edit::assembly::Pose pose{.x = world_x, .y = world_y, .heading = 0.0};
    action.command = is_t ? edit::assembly::t_intersection(network, pose)
                          : edit::assembly::x_intersection(network, pose);
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Assembly;
      action.preview = {world_x, world_y, true}; // standalone at the cursor
      const bool near_road = on_road.has_value();
      action.toast = near_road ? QStringLiteral("Dropped near a road end — placed a standalone "
                                                "intersection instead; Ctrl+Z to undo")
                     : is_t    ? QStringLiteral("Placed T-intersection — Ctrl+Z to undo")
                               : QStringLiteral("Placed X-intersection — Ctrl+Z to undo");
    }
    return action;
  }
  case LibraryItem::Kind::Tree: {
    // Props are road-relative: snap to the nearest road, or reject with a hint
    // (OpenDRIVE has no world-placed object).
    const auto placement = nearest_road_station(network, world_x, world_y, kTreeSnapThreshold);
    if (!placement.has_value()) {
      action.toast = QStringLiteral("Drop a tree onto or beside a road");
      return action; // kind stays None, preview invalid at cursor — caller hints
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
      // Ghost at the exact station the object occupies — the same (road, s, t)
      // → world projection the mesh builder uses, so ghost==commit.
      if (const Road* road = network.road(placement->road)) {
        const auto p = station_to_world(road->plan_view, placement->s, placement->t);
        action.preview = {p[0], p[1], true};
      }
      action.toast = QStringLiteral("Placed %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
  case LibraryItem::Kind::Signal: {
    // Signals are road-relative like props: snap to the nearest road, or reject
    // with a hint. The dropped (s, t) is where the pole plants — the same
    // (road, s, t) → world projection the mesh builder instances the signal at,
    // so the ghost marks exactly where it lands (ghost==commit).
    const auto placement = nearest_road_station(network, world_x, world_y, kSignalSnapThreshold);
    if (!placement.has_value()) {
      action.toast = QStringLiteral("Drop a signal onto or beside a road");
      return action; // kind stays None, preview invalid — caller hints
    }
    const bool light = item.signal != QStringLiteral("sign");
    Signal signal = make_dropped_signal(network, light, placement->s, placement->t);
    action.command = edit::add_signal(network, placement->road, std::move(signal));
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Signal;
      if (const Road* road = network.road(placement->road)) {
        const auto p = station_to_world(road->plan_view, placement->s, placement->t);
        action.preview = {p[0], p[1], true};
      }
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
