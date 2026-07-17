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
/// drop in open space is rejected (OpenDRIVE objects are road-relative). Shared
/// with the prop move-drag so dropping and dragging agree on where the road ends.
constexpr double kTreeSnapThreshold = kObjectSnapThreshold;

/// A T/X assembly dropped within this lateral distance [m] of a road's
/// reference line tees/crosses INTO that road (aligned); a drop farther out is
/// a standalone intersection at the cursor.
constexpr double kAssemblySnapThreshold = 8.0;

/// A road style applies to the nearest road whose reference line passes within
/// this lateral distance [m] of the drop — wide enough to grab a road by
/// dropping anywhere across its carriageway, since the style targets the whole
/// road, not a point on it.
constexpr double kRoadStyleSnapThreshold = 20.0;

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
/// A marking snaps to a lane boundary within this lateral distance [m] of a
/// road's reference line — the whole carriageway is in reach so a drop anywhere
/// across it grabs the nearest boundary; a drop in open space is rejected.
constexpr double kMarkingSnapThreshold = 12.0;

/// The RoadMarkType for a manifest mark_type string, or nullopt for a spelling
/// this build does not paint (forward-compatible with #220 variants: an unknown
/// type rejects the drop rather than silently mis-marking).
std::optional<RoadMarkType> mark_type_from_string(const QString& type) {
  if (type == QStringLiteral("solid")) {
    return RoadMarkType::Solid;
  }
  if (type == QStringLiteral("broken")) {
    return RoadMarkType::Broken;
  }
  if (type == QStringLiteral("solid_solid")) {
    return RoadMarkType::SolidSolid;
  }
  if (type == QStringLiteral("solid_broken")) {
    return RoadMarkType::SolidBroken;
  }
  if (type == QStringLiteral("broken_solid")) {
    return RoadMarkType::BrokenSolid;
  }
  return std::nullopt;
}

/// The RoadMarkColor for a manifest mark_color string, or nullopt for an unknown
/// spelling. An empty/absent color means "the standard color for this type".
std::optional<RoadMarkColor> mark_color_from_string(const QString& color) {
  if (color.isEmpty() || color == QStringLiteral("standard")) {
    return RoadMarkColor::Standard;
  }
  if (color == QStringLiteral("white")) {
    return RoadMarkColor::White;
  }
  if (color == QStringLiteral("yellow")) {
    return RoadMarkColor::Yellow;
  }
  if (color == QStringLiteral("red")) {
    return RoadMarkColor::Red;
  }
  if (color == QStringLiteral("blue")) {
    return RoadMarkColor::Blue;
  }
  if (color == QStringLiteral("green")) {
    return RoadMarkColor::Green;
  }
  if (color == QStringLiteral("orange")) {
    return RoadMarkColor::Orange;
  }
  return std::nullopt;
}

/// The RoadMark a Marking library item authors, or nullopt when its type/color
/// spelling is unknown. `lines` stays empty (compact form — the mesher
/// synthesizes multi-stripe geometry for solid_solid etc.), mirroring the
/// junction centre-double-yellow precedent.
std::optional<RoadMark> mark_from_item(const LibraryItem& item) {
  const auto type = mark_type_from_string(item.mark_type);
  const auto color = mark_color_from_string(item.mark_color);
  if (!type.has_value() || !color.has_value()) {
    return std::nullopt;
  }
  RoadMark mark;
  mark.s_offset = 0.0;
  mark.type = *type;
  mark.width = item.mark_width;
  mark.color = *color;
  return mark;
}

/// The LaneId of the lane with OpenDRIVE `odr_id` in the section governing
/// station `s` on `road` (odr 0 = centre line). Invalid id when not found.
LaneId lane_for_odr_id(const RoadNetwork& network, RoadId road, double s, int odr_id) {
  const LaneSection* section = network.lane_section(section_at(network, road, s));
  if (section == nullptr) {
    return {};
  }
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane != nullptr && lane->odr_id == odr_id) {
      return lane_id;
    }
  }
  return {};
}

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

RoadStyle style_for(const QString& name) {
  // Accepts both the manifest style name ("urban_two_lane", from the drop
  // resolver) and the library item key ("style.urban", from the Attributes
  // slot, which passes the dropped key straight through). urban is the default.
  if (name == QStringLiteral("two_lane_rural") || name == QStringLiteral("style.rural")) {
    return RoadStyle::two_lane_rural();
  }
  if (name == QStringLiteral("highway") || name == QStringLiteral("style.highway")) {
    return RoadStyle::highway();
  }
  return RoadStyle::urban_two_lane();
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
  case LibraryItem::Kind::RoadStyle: {
    // Drop ONTO a road applies the style to it. Resolve the road under the
    // cursor; report it in target_road so the drag can highlight it. A style
    // never targets a junction connecting road (apply_road_style refuses it).
    const auto on_road = nearest_road_station(network, world_x, world_y, kRoadStyleSnapThreshold);
    if (!on_road.has_value()) {
      action.toast = QStringLiteral("Drop a road style onto a road");
      return action; // kind None, preview invalid at the cursor — caller hints
    }
    const Road* road = network.road(on_road->road);
    if (road != nullptr && road->junction.is_valid()) {
      action.toast = QStringLiteral("Road styles can't be applied to junction connecting roads");
      return action;
    }
    action.command = edit::apply_road_style(network, on_road->road, style_for(item.style));
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::RoadStyle;
      action.target_road = on_road->road;
      if (road != nullptr) {
        const auto p = station_to_world(road->plan_view, on_road->s, 0.0);
        action.preview = {p[0], p[1], true};
      }
      action.toast = QStringLiteral("Applied %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
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
  case LibraryItem::Kind::Marking: {
    // A marking paints one lane boundary in the section under the cursor. Snap
    // to the nearest road, then to the nearest lane boundary within it; a drop
    // in open space is rejected with a hint.
    const auto on_road = nearest_road_station(network, world_x, world_y, kMarkingSnapThreshold);
    if (!on_road.has_value()) {
      action.toast = QStringLiteral("Drop a marking onto a lane boundary");
      return action; // kind None, preview invalid at cursor — caller hints
    }
    const std::optional<RoadMark> mark = mark_from_item(item);
    if (!mark.has_value()) {
      action.toast = QStringLiteral("That marking style isn't supported yet");
      return action;
    }
    const Road* road = network.road(on_road->road);
    if (road == nullptr) {
      return action;
    }
    const auto boundary = nearest_lane_boundary(network, on_road->road, on_road->s, on_road->t);
    if (!boundary.has_value()) {
      action.toast = QStringLiteral("Drop a marking onto a lane boundary");
      return action;
    }
    // A lane's mark list describes its OUTER boundary; the centre line is lane
    // odr 0 (nearest_lane_boundary reports at_odr_id = ±1 there for insert
    // semantics, so the centre flag re-targets it to lane 0).
    const int target_odr = boundary->centre ? 0 : boundary->at_odr_id;
    const LaneId lane = lane_for_odr_id(network, on_road->road, on_road->s, target_odr);
    if (!lane.is_valid()) {
      return action;
    }
    // A drop that changes nothing pushes no command.
    if (const Lane* target = network.lane(lane);
        target != nullptr && !target->road_marks.empty() && target->road_marks.front() == *mark) {
      action.toast = QStringLiteral("That boundary already has this marking");
      return action;
    }
    action.command = edit::set_road_mark(network, lane, *mark);
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Marking;
      // Ghost sits ON the picked boundary (ghost==commit): the same station the
      // mark applies at, offset to the boundary's lateral t.
      const auto p = station_to_world(road->plan_view, on_road->s, boundary->t);
      action.preview = {p[0], p[1], true};
      action.toast = QStringLiteral("Painted %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
  case LibraryItem::Kind::Material:
    // Materials attach to a Surface via the Attributes-pane slot, not a world
    // point — a viewport drop can't resolve a target, so hint at the slot.
    action.toast = QStringLiteral("Drop the material onto a Material slot in the Attributes pane");
    return action;
  case LibraryItem::Kind::Unknown:
    break;
  }
  return action;
}

} // namespace roadmaker::editor
