#include "document/library_drop.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>

#include "document/crosswalk_item.hpp"
#include "document/crosswalk_placement.hpp"
#include "document/marking_item.hpp"
#include "document/prop_placement.hpp"
#include "document/signal_placement.hpp"
#include "document/stencil_placement.hpp"
#include "render/material_catalog.hpp"
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

/// RoadStation, nearest_road_station, and next_object_odr_id now live in
/// document/prop_placement.hpp (shared with the prop tools); this TU uses them
/// via that include so the drop and the tools snap and mint ids identically.

/// Signal snapping and id-minting now live in document/signal_placement.hpp
/// (shared with the Signal tool, p4-s7); this TU uses them via that include so
/// the drop and the tool snap and mint ids identically.

/// A marking snaps to a lane boundary within this lateral distance [m] of a
/// road's reference line — the whole carriageway is in reach so a drop anywhere
/// across it grabs the nearest boundary; a drop in open space is rejected.
constexpr double kMarkingSnapThreshold = 12.0;

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

/// A material drop snaps to a road this far [m] from its reference line, then
/// resolves the containing lane below; wide enough to grab a road by dropping
/// anywhere across its carriageway (lane_containing_t rejects off-lane drops).
constexpr double kMaterialSnapThreshold = 20.0;

/// A material drop closer than this [m] to the centre line is treated as "on
/// the centre line" and rejected: the centre lane carries no material by rule,
/// and both adjacent lanes are equally near, so ask the user to aim at a lane.
/// Far narrower than any real lane, so it never eats a genuine lane drop.
constexpr double kCentreDeadZone = 0.2;

/// The LaneId of the non-centre lane whose lateral band contains `cursor_t` at
/// station `s`, or an invalid id when the cursor is on the centre line or off
/// the carriageway. Bands come from lane_boundary_offsets (leftmost-first: left
/// outer edges, the centre boundary, then right edges); band [i, i+1] is lane
/// +(nleft - i) on the left and lane -(i - nleft + 1) on the right.
LaneId lane_containing_t(const RoadNetwork& network, RoadId road, double s, double cursor_t) {
  const std::vector<double> offsets = lane_boundary_offsets(network, road, s);
  if (offsets.size() < 2) {
    return {};
  }
  const LaneSection* section = network.lane_section(section_at(network, road, s));
  if (section == nullptr) {
    return {};
  }
  int nleft = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane != nullptr && lane->odr_id > 0) {
      ++nleft;
    }
  }
  // Reject a drop hugging the centre line (the centre boundary is offsets[nleft]).
  if (std::abs(cursor_t - offsets[static_cast<std::size_t>(nleft)]) < kCentreDeadZone) {
    return {};
  }
  for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
    const double hi = std::max(offsets[i], offsets[i + 1]);
    const double lo = std::min(offsets[i], offsets[i + 1]);
    if (cursor_t <= hi && cursor_t >= lo) {
      const int index = static_cast<int>(i);
      const int odr = index < nleft ? nleft - index : -(index - nleft + 1);
      return lane_for_odr_id(network, road, s, odr); // odr is never 0 here
    }
  }
  return {}; // off the carriageway
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
    // The bundled model is the single source of truth for the object class
    // (Tree/Vegetation/Pole/Building) and dimensions; an unknown model → Tree.
    tree.type = ObjectType::Tree;
    tree.s = placement->s;
    tree.t = placement->t;
    if (const props::PropModel* model = props::model(tree.name)) {
      tree.type = model->type;
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
    const auto placement = nearest_signal_station(network, world_x, world_y);
    if (!placement.has_value()) {
      action.toast = QStringLiteral("Drop a signal onto or beside a road");
      return action; // kind stays None, preview invalid — caller hints
    }
    Signal signal =
        make_signal(item.signal, next_signal_odr_id(network), placement->s, placement->t);
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
  case LibraryItem::Kind::Material: {
    // A material paints the lane surface under the cursor: snap to the nearest
    // road, then to the lane whose band contains the cursor t. A drop on the
    // centre line or in open space is rejected with a hint.
    const auto on_road = nearest_road_station(network, world_x, world_y, kMaterialSnapThreshold);
    if (!on_road.has_value()) {
      action.toast = QStringLiteral("Drop a material onto a lane");
      return action;
    }
    const MaterialCatalog catalog;
    const MaterialDef* def = catalog.find_material(item.material.toStdString());
    if (def == nullptr) {
      action.toast = QStringLiteral("That material isn't available yet");
      return action;
    }
    const Road* road = network.road(on_road->road);
    if (road == nullptr) {
      return action;
    }
    const LaneId lane = lane_containing_t(network, on_road->road, on_road->s, on_road->t);
    if (!lane.is_valid()) {
      action.toast = QStringLiteral("Drop a material onto a lane (not the centre line)");
      return action;
    }
    // One record replaces the whole profile — the same first-record
    // simplification set_road_mark uses (documented on the op): a viewport drop
    // authors a single constant material, not a multi-record profile. Author
    // "rm:<name>" + the catalog's nominal friction.
    const LaneMaterial record{
        .s_offset = 0.0, .friction = def->friction, .surface = "rm:" + item.material.toStdString()};
    if (const Lane* target = network.lane(lane);
        target != nullptr && target->materials.size() == 1 && target->materials.front() == record) {
      action.toast = QStringLiteral("That lane already has this material");
      return action;
    }
    action.command = edit::set_lane_material(network, lane, {record});
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Material;
      // Ghost sits ON the picked lane point (ghost==commit).
      const auto p = station_to_world(road->plan_view, on_road->s, on_road->t);
      action.preview = {p[0], p[1], true};
      action.toast = QStringLiteral("Applied %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
  case LibraryItem::Kind::Crosswalk: {
    // A crosswalk asset dropped onto a junction approach places a crosswalk +
    // its stop line along that arm's lane cross-section (GW-2 step 10). The arm
    // is resolved from the cursor exactly as the interactive tool does, so both
    // paths land the same objects. A drop away from any approach is rejected.
    const auto arm = nearest_junction_arm(network, world_x, world_y, kCrosswalkSnapThreshold);
    if (!arm.has_value()) {
      action.toast = QStringLiteral("Drop a crosswalk onto a junction approach");
      return action; // kind None, preview invalid at the cursor — caller hints
    }
    const MaterialCatalog catalog;
    const edit::CrosswalkParams params = crosswalk_params_from_item(item, catalog);
    std::optional<std::pair<RoadId, Object>> placed =
        crosswalk_for_arm(network, arm->junction, arm->arm_road, params);
    if (!placed.has_value()) {
      action.toast = QStringLiteral("That approach has no lanes to cross");
      return action;
    }
    // The stop line is derived, not placed: record the link so the drop's macro
    // authors the same setback + provenance the interactive tool does.
    action.stopline_link =
        StopLineLink{.junction = arm->junction,
                     .arm = RoadEnd{.road = arm->arm_road, .contact = arm->contact},
                     .crosswalk_odr_id = placed->second.odr_id};
    action.objects.push_back(*std::move(placed));
    action.kind = LibraryDropKind::Crosswalk;
    // Ghost sits where the arm meets the junction (ghost==commit); the tool's
    // chevron marks the same spot while the drag is live.
    action.preview = {arm->anchor_x, arm->anchor_y, true};
    action.toast = QStringLiteral("Placed %1 + stop line — Ctrl+Z to undo").arg(item.label);
    return action;
  }
  case LibraryItem::Kind::Stencil: {
    // A stencil asset dropped onto a lane places ONE arrow glyph at the picked
    // station, oriented to the lane's travel direction (GW-5 step 6). The lane
    // is resolved from the cursor exactly as the interactive Marking Point tool
    // does, so both paths land the same object. A drop off any lane is rejected.
    const MaterialCatalog catalog;
    std::optional<std::pair<RoadId, Object>> placed =
        stencil_for_point(network, world_x, world_y, item, catalog);
    if (!placed.has_value()) {
      action.toast = QStringLiteral("Drop an arrow stencil onto a lane");
      return action; // kind None, preview invalid at the cursor — caller hints
    }
    const RoadId road = placed->first;
    // Ghost at the exact station the object occupies (ghost==commit), captured
    // before the object moves into add_object.
    if (const Road* r = network.road(road)) {
      const auto p = station_to_world(r->plan_view, placed->second.s, placed->second.t);
      action.preview = {p[0], p[1], true};
    }
    action.command = edit::add_object(network, road, std::move(placed->second));
    if (action.command != nullptr) {
      action.kind = LibraryDropKind::Stencil;
      action.toast = QStringLiteral("Placed %1 — Ctrl+Z to undo").arg(item.label);
    }
    return action;
  }
  case LibraryItem::Kind::PropSet:
    // A prop set is authored/scattered through the prop tools (p6-s5), not
    // dropped straight onto the viewport — no direct drop action this round.
    break;
  case LibraryItem::Kind::Unknown:
    break;
  }
  return action;
}

} // namespace roadmaker::editor
