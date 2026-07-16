#include "soak/soak_driver.hpp"

#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/xodr/reader.hpp"
#include "roadmaker/xodr/writer.hpp"

#include <fmt/format.h>

#include <QElapsedTimer>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <numbers>
#include <utility>
#include <vector>

#include "document/elevation_utils.hpp"

namespace roadmaker::editor::soak {

SoakDriver::SoakDriver(Document& document, SelectionModel& selection, SoakOptions options)
    : document_(document), selection_(selection), options_(std::move(options)),
      rng_(options_.seed) {}

bool SoakDriver::run() {
  QElapsedTimer timer;
  timer.start();
  int index = 0;
  while (failure_.empty()) {
    if (options_.max_ops > 0 && index >= options_.max_ops) {
      break;
    }
    if (options_.max_seconds > 0.0 &&
        static_cast<double>(timer.elapsed()) / 1000.0 >= options_.max_seconds) {
      break;
    }
    step(index);
    ++index;
  }
  return failure_.empty();
}

void SoakDriver::step(int index) {
  // Weighted operation table — creation is favored early so the network
  // grows; the weights matter less than every path being exercised.
  struct WeightedOp {
    int weight;
    void (SoakDriver::*op)();
    const char* label;
  };

  static constexpr WeightedOp kOps[] = {
      {3, &SoakDriver::op_create_road, "create_road"},
      {5, &SoakDriver::op_drag_waypoint, "drag_waypoint"},
      {1, &SoakDriver::op_insert_waypoint, "insert_waypoint"},
      {1, &SoakDriver::op_delete_waypoint, "delete_waypoint"},
      {2, &SoakDriver::op_lane_edit, "lane_edit"},
      {2, &SoakDriver::op_split_lane_section, "split_lane_section"},
      {2, &SoakDriver::op_lane_width_profile, "lane_width_profile"},
      {1, &SoakDriver::op_elevation, "elevation"},
      {1, &SoakDriver::op_split_road, "split_road"},
      {2, &SoakDriver::op_translate_road, "translate_road"},
      {2, &SoakDriver::op_rotate_road, "rotate_road"},
      {1, &SoakDriver::op_merge_roads, "merge_roads"},
      {2, &SoakDriver::op_create_junction, "create_junction"},
      {1, &SoakDriver::op_duplicate_junction_attempt, "duplicate_junction_attempt"},
      {1, &SoakDriver::op_attach_t, "attach_t"},
      {1, &SoakDriver::op_assembly_drop_on_road, "assembly_drop_on_road"},
      {1, &SoakDriver::op_extend_road, "extend_road"},
      {1, &SoakDriver::op_cross_commit, "cross_commit"},
      {1, &SoakDriver::op_tee_commit, "tee_commit"},
      {1, &SoakDriver::op_remove_lane, "remove_lane"},
      {2, &SoakDriver::op_insert_lane, "insert_lane"},
      {2, &SoakDriver::op_lane_add_span, "lane_add_span"},
      {2, &SoakDriver::op_lane_form, "lane_form"},
      {2, &SoakDriver::op_ground_surface, "ground_surface"},
      {1, &SoakDriver::op_overpass, "overpass"},
      {1, &SoakDriver::op_delete_crossing_road, "delete_crossing_road"},
      {1, &SoakDriver::op_delete_junction, "delete_junction"},
      {1, &SoakDriver::op_delete_road, "delete_road"},
      {2, &SoakDriver::op_undo_redo, "undo_redo"},
      {1, &SoakDriver::op_save_reload, "save_reload"},
      {1, &SoakDriver::op_select, "select"},
  };
  int total = 0;
  for (const WeightedOp& op : kOps) {
    total += op.weight;
  }
  int pick = rand_int(1, total);
  const WeightedOp* chosen = &kOps[0];
  for (const WeightedOp& op : kOps) {
    pick -= op.weight;
    if (pick <= 0) {
      chosen = &op;
      break;
    }
  }

  current_label_ = chosen->label;
  (this->*chosen->op)();
  ++stats_.ops;
  if (!check_invariants(index, chosen->label)) {
    return;
  }
  if (options_.round_trip_every > 0 && (index + 1) % options_.round_trip_every == 0) {
    // The cadence-based full round-trip; save_reload runs its own.
    const auto first = write_xodr(document_.network(), "soak");
    if (!first) {
      // An unwritable network right after a passing validate is itself a
      // finding.
      fail(
          index, chosen->label, "write_xodr refused a validated network: " + first.error().message);
      return;
    }
    auto parsed = parse_xodr(*first, "<soak>");
    if (!parsed) {
      fail(index, chosen->label, "parse of own output failed: " + parsed.error().message);
      return;
    }
    for (const Diagnostic& diagnostic : parsed->diagnostics) {
      if (diagnostic.severity == Severity::Error) {
        fail(index, chosen->label, "own output parses with error: " + diagnostic.message);
        return;
      }
    }
    const auto second = write_xodr(parsed->network, "soak");
    if (!second) {
      fail(index, chosen->label, "re-write after parse failed: " + second.error().message);
      return;
    }
    if (*first != *second) {
      // Leave both generations on disk — the diff IS the bug report.
      dump_round_trip(*first, *second);
      fail(index,
           chosen->label,
           fmt::format("write→parse→write is not byte-identical (dumped to {})",
                       options_.work_dir.string()));
      return;
    }
  }
}

bool SoakDriver::check_invariants(int index, const char* label) {
  if (document_.preview_active()) {
    fail(index, label, "preview session leaked out of the operation");
    return false;
  }

  const RoadNetwork& network = document_.network();

  // Structural resolve: everything the model references must exist.
  std::string broken;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    if (!broken.empty()) {
      return;
    }
    if (road.junction.is_valid() && network.junction(road.junction) == nullptr) {
      broken = fmt::format("road {} references dead junction", road.odr_id);
      return;
    }
    for (const LaneSectionId section_id : road.sections) {
      const LaneSection* section = network.lane_section(section_id);
      if (section == nullptr) {
        broken = fmt::format("road {} references dead lane section", road.odr_id);
        return;
      }
      for (const LaneId lane_id : section->lanes) {
        if (network.lane(lane_id) == nullptr) {
          broken = fmt::format("road {} references dead lane", road.odr_id);
          return;
        }
      }
    }
    (void)road_id;
  });
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    if (!broken.empty()) {
      return;
    }
    for (const JunctionConnection& connection : junction.connections) {
      if (network.road(connection.incoming_road) == nullptr ||
          network.road(connection.connecting_road) == nullptr) {
        broken = fmt::format("junction {} connection references dead road", junction.odr_id);
        return;
      }
    }
    for (const RoadEnd& arm : junction.arms) {
      if (network.road(arm.road) == nullptr) {
        broken = fmt::format("junction {} arm references dead road", junction.odr_id);
        return;
      }
    }
    (void)junction_id;
  });
  if (!broken.empty()) {
    fail(index, label, broken);
    return false;
  }

  // Every rendered primitive maps to a selectable entity (gate finding 4): a
  // junction floor renders as its own pickable surface, so its JunctionId must
  // resolve — a floor left behind for a dead junction would be an
  // unselectable "ghost" area.
  for (const JunctionFloor& floor : document_.mesh().junction_floors) {
    if (network.junction(floor.junction) == nullptr) {
      fail(index, label, "rendered junction floor references a dead junction");
      return false;
    }
  }

  // Selection must never hold stale ids (02 §1: all flows through
  // SelectionModel, which prunes on topology changes).
  for (const SelectionEntry& entry : selection_.entries()) {
    if (entry.junction.is_valid()) {
      if (network.junction(entry.junction) == nullptr) {
        fail(index, label, "selection holds a stale junction id");
        return false;
      }
      continue;
    }
    if (network.road(entry.road) == nullptr) {
      fail(index, label, "selection holds a stale road id");
      return false;
    }
    if (entry.lane.is_valid() && network.lane(entry.lane) == nullptr) {
      fail(index, label, "selection holds a stale lane id");
      return false;
    }
  }

  // The validator is the standards-level invariant.
  for (const Diagnostic& diagnostic : validate_network(network)) {
    if (diagnostic.severity == Severity::Error) {
      fail(index, label, "validate_network error: " + diagnostic.message);
      return false;
    }
  }
  return true;
}

void SoakDriver::fail(int index, const char* label, const std::string& detail) {
  failure_ =
      fmt::format("soak failure: seed={} op#{} ({}): {}", options_.seed, index, label, detail);
}

void SoakDriver::dump_round_trip(const std::string& first, const std::string& second) const {
  std::ofstream(options_.work_dir / "round_trip_first.xodr") << first;
  std::ofstream(options_.work_dir / "round_trip_second.xodr") << second;
}

// --- operations --------------------------------------------------------------

void SoakDriver::push(std::unique_ptr<edit::Command> command) {
  if (document_.push_command(std::move(command)).has_value()) {
    ++stats_.commands;
  } else {
    ++stats_.rejected;
  }
}

void SoakDriver::op_create_road() {
  const int count = rand_int(2, 5);
  std::vector<Waypoint> waypoints;
  waypoints.reserve(static_cast<std::size_t>(count));
  double x = rand_range(-400.0, 400.0);
  double y = rand_range(-400.0, 400.0);
  double heading = rand_range(0.0, 2.0 * std::numbers::pi);
  waypoints.push_back(Waypoint{.x = x, .y = y});
  for (int i = 1; i < count; ++i) {
    heading += rand_range(-0.6, 0.6); // gentle turns keep the fit valid-ish
    const double advance = rand_range(30.0, 90.0);
    x += advance * std::cos(heading);
    y += advance * std::sin(heading);
    waypoints.push_back(Waypoint{.x = x, .y = y});
  }
  push(edit::create_road(std::move(waypoints), random_profile(), {}));
}

void SoakDriver::op_drag_waypoint() {
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  const std::vector<Waypoint> waypoints = edit::effective_waypoints(*road);
  if (waypoints.size() < 2) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(rand_int(0, int(waypoints.size()) - 1));

  // A drag: begin, a few move updates, then commit (usually) or Esc.
  Waypoint target = waypoints[index];
  if (chance(0.1) && waypoints.size() >= 2) {
    // Sharp reversal: drag the node next to a neighbor — the runaway-fit
    // class the maintainer hit (#93); the command layer must refuse, never
    // balloon.
    const std::size_t neighbor = index > 0 ? index - 1 : index + 1;
    target.x = waypoints[neighbor].x + rand_range(-6.0, 6.0);
    target.y = waypoints[neighbor].y + rand_range(-6.0, 6.0);
  } else {
    target.x += rand_range(-25.0, 25.0);
    target.y += rand_range(-25.0, 25.0);
  }
  if (!document_.begin_preview(edit::move_waypoint(document_.network(), road_id, index, target))
           .has_value()) {
    ++stats_.rejected;
    return;
  }
  const int updates = rand_int(0, 3);
  for (int i = 0; i < updates; ++i) {
    target.x += rand_range(-10.0, 10.0);
    target.y += rand_range(-10.0, 10.0);
    const auto updated = document_.update_preview(
        [&](const RoadNetwork& base) { return edit::move_waypoint(base, road_id, index, target); });
    if (!updated.has_value()) {
      break; // session degraded gracefully or ended; invariants judge below
    }
  }
  ++stats_.previews;
  if (!document_.preview_active()) {
    return; // a failed update tore the session down; nothing to end
  }
  if (chance(0.2)) {
    document_.cancel_preview();
  } else {
    document_.commit_preview();
    ++stats_.commands;
  }
}

void SoakDriver::op_insert_waypoint() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  const std::vector<Waypoint> waypoints = edit::effective_waypoints(*road);
  const std::size_t index = static_cast<std::size_t>(rand_int(0, int(waypoints.size())));
  const Waypoint& anchor = waypoints[std::min(index, waypoints.size() - 1)];
  push(edit::insert_waypoint(
      document_.network(),
      road_id,
      index,
      Waypoint{.x = anchor.x + rand_range(-50.0, 50.0), .y = anchor.y + rand_range(-50.0, 50.0)}));
}

void SoakDriver::op_delete_waypoint() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  const std::vector<Waypoint> waypoints = edit::effective_waypoints(*road);
  if (waypoints.size() < 3) {
    return; // a 2-waypoint road must keep both ends
  }
  push(edit::delete_waypoint(document_.network(),
                             road_id,
                             static_cast<std::size_t>(rand_int(0, int(waypoints.size()) - 1))));
}

/// Cuts a lane section at a random station. Splitting where a section already
/// starts is a legal no-op, so the driver deliberately does not avoid
/// boundaries — hitting one exercises the idempotent path.
void SoakDriver::op_split_lane_section() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->length <= 0.0) {
    return;
  }
  // Deliberately unclamped at the ends: the kernel must refuse a degenerate
  // station rather than produce a zero-length section.
  push(
      edit::split_lane_section(document_.network(), road_id, rand_range(-1.0, road->length + 1.0)));
}

/// Authors a width profile that varies along s — the shape Lane Carve
/// produces. Profiles are built to be conformant about as often as not, so
/// both the accept and the refuse path get exercised.
void SoakDriver::op_lane_width_profile() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return;
  }
  const LaneSectionId section_id =
      road->sections[static_cast<std::size_t>(rand_int(0, int(road->sections.size()) - 1))];
  const LaneSection* section = document_.network().lane_section(section_id);
  if (section == nullptr || section->lanes.empty()) {
    return;
  }
  const LaneId lane =
      section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];

  std::vector<roadmaker::Poly3> widths;
  // Zero width is legal and is how a turn lane starts, so `a` may be 0.
  widths.push_back(roadmaker::Poly3{.s = 0.0,
                                    .a = chance(0.2) ? 0.0 : rand_range(1.0, 4.0),
                                    .b = chance(0.5) ? rand_range(-0.05, 0.05) : 0.0});
  if (chance(0.5)) {
    widths.push_back(roadmaker::Poly3{.s = rand_range(-5.0, road->length),
                                      .a = rand_range(-0.5, 4.0),
                                      .b = rand_range(-0.05, 0.05)});
  }
  push(edit::set_lane_width_profile(document_.network(), lane, std::move(widths)));
}

void SoakDriver::op_lane_edit() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road->sections.empty()) {
    return;
  }
  const LaneSectionId section_id =
      road->sections[static_cast<std::size_t>(rand_int(0, int(road->sections.size()) - 1))];
  const LaneSection* section = document_.network().lane_section(section_id);

  switch (rand_int(0, 3)) {
  case 0: {
    static constexpr LaneType kTypes[] = {
        LaneType::Driving, LaneType::Sidewalk, LaneType::Shoulder, LaneType::Parking};
    push(edit::add_lane(
        document_.network(), section_id, chance(0.5) ? 1 : -1, kTypes[rand_int(0, 3)]));
    break;
  }
  case 1: {
    if (section->lanes.empty()) {
      return;
    }
    const LaneId lane =
        section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
    push(edit::remove_lane(document_.network(), lane));
    break;
  }
  case 2: {
    if (section->lanes.empty()) {
      return;
    }
    const LaneId lane =
        section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
    push(edit::set_lane_width(document_.network(), lane, rand_range(2.0, 5.0)));
    break;
  }
  default: {
    if (section->lanes.empty()) {
      return;
    }
    const LaneId lane =
        section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
    static constexpr LaneType kTypes[] = {
        LaneType::Driving, LaneType::Sidewalk, LaneType::Biking, LaneType::Border};
    push(edit::set_lane_type(document_.network(), lane, kTypes[rand_int(0, 3)]));
    break;
  }
  }
}

void SoakDriver::op_elevation() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  const std::vector<Waypoint> waypoints = edit::effective_waypoints(*road);
  if (waypoints.empty()) {
    return;
  }
  push(edit::set_node_elevation(document_.network(),
                                road_id,
                                static_cast<std::size_t>(rand_int(0, int(waypoints.size()) - 1)),
                                rand_range(-8.0, 8.0)));
}

void SoakDriver::op_split_road() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road->length < 10.0) {
    return;
  }
  push(edit::split_road(document_.network(), road_id, rand_range(2.0, road->length - 2.0)));
}

void SoakDriver::op_translate_road() {
  std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  // Move one or two distinct roads together by a small random delta. The
  // command refuses junction-touching roads, which push() records as rejected.
  const int count = std::min<int>(rand_int(1, 2), static_cast<int>(roads.size()));
  std::vector<RoadId> moved;
  for (int i = 0; i < count; ++i) {
    const RoadId candidate = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
    if (std::ranges::find(moved, candidate) == moved.end()) {
      moved.push_back(candidate);
    }
  }
  push(edit::translate_roads(
      document_.network(), moved, rand_range(-20.0, 20.0), rand_range(-20.0, 20.0)));
}

void SoakDriver::op_rotate_road() {
  // Gizmo-style yaw: rotate one road about its mid-point by a random angle. The
  // command refuses junction-touching roads, which push() records as rejected.
  std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road_ptr = document_.network().road(road);
  if (road_ptr == nullptr || road_ptr->plan_view.empty()) {
    return;
  }
  const PathPoint pivot = road_ptr->plan_view.evaluate(road_ptr->plan_view.length() / 2.0);
  push(edit::rotate_road(
      document_.network(), road, rand_range(-3.14159, 3.14159), pivot.x, pivot.y));
}

void SoakDriver::op_merge_roads() {
  // Merge the first road whose successor is a mergeable plain-road neighbor
  // (commonly a pair produced by an earlier split).
  for (const RoadId a : live_roads(/*editable_only=*/true)) {
    const Road* road = document_.network().road(a);
    if (road == nullptr || !road->successor.has_value()) {
      continue;
    }
    const auto* b = std::get_if<RoadId>(&road->successor->target);
    if (b == nullptr) {
      continue;
    }
    if (edit::check_mergeable(document_.network(), a, *b).has_value()) {
      push(edit::merge_roads(document_.network(), a, *b));
      return;
    }
  }
}

void SoakDriver::op_create_junction() {
  // Free ends: not a connecting road, and the matching link slot is empty.
  struct FreeEnd {
    RoadEnd end;
    double x;
    double y;
  };

  std::vector<FreeEnd> ends;
  document_.network().for_each_road([&](RoadId road_id, const Road& road) {
    if (road.junction.is_valid()) {
      return;
    }
    if (!road.predecessor.has_value()) {
      const PathPoint point = road.plan_view.evaluate(0.0);
      ends.push_back(FreeEnd{.end = RoadEnd{.road = road_id, .contact = ContactPoint::Start},
                             .x = point.x,
                             .y = point.y});
    }
    if (!road.successor.has_value()) {
      const PathPoint point = road.plan_view.evaluate(road.length);
      ends.push_back(FreeEnd{.end = RoadEnd{.road = road_id, .contact = ContactPoint::End},
                             .x = point.x,
                             .y = point.y});
    }
  });
  if (ends.size() < 2) {
    return;
  }
  const FreeEnd& anchor = ends[static_cast<std::size_t>(rand_int(0, int(ends.size()) - 1))];
  std::vector<RoadEnd> arms{anchor.end};
  for (const FreeEnd& candidate : ends) {
    if (candidate.end == anchor.end || candidate.end.road == anchor.end.road) {
      continue; // one arm per road keeps the generated turn set sane
    }
    if (std::hypot(candidate.x - anchor.x, candidate.y - anchor.y) < 45.0) {
      arms.push_back(candidate.end);
      if (arms.size() == 4) {
        break;
      }
    }
  }
  if (arms.size() < 2) {
    return;
  }
  push(edit::create_junction(document_.network(), arms));
}

void SoakDriver::op_duplicate_junction_attempt() {
  // Re-attempt a junction over the EXACT arms of an existing one: the
  // single-owner invariant must refuse it (gate finding 5), leaving the network
  // byte-unchanged (a refused command never mutates). A wrongly-accepted
  // duplicate would trip validate_network's arm_single_owner rule in
  // check_invariants.
  const std::vector<JunctionId> junctions = live_junctions();
  if (junctions.empty()) {
    return;
  }
  const Junction* junction = document_.network().junction(
      junctions[static_cast<std::size_t>(rand_int(0, int(junctions.size()) - 1))]);
  if (junction == nullptr || junction->arms.size() < 2) {
    return; // foreign junctions carry no arms to re-attempt
  }
  push(edit::create_junction(document_.network(), junction->arms));
}

void SoakDriver::op_attach_t() {
  // Tee a road end into another road's side (the hardening T workflow).
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.size() < 2) {
    return;
  }
  const RoadId attach_road = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  RoadId target_road = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  if (target_road == attach_road) {
    return; // self-tee is a rejection path already covered by kernel tests
  }
  const Road* target = document_.network().road(target_road);
  if (target->length < 30.0) {
    return;
  }
  const RoadEnd end{.road = attach_road,
                    .contact = chance(0.5) ? ContactPoint::Start : ContactPoint::End};
  push(edit::attach_t_junction(
      document_.network(), end, target_road, rand_range(12.0, target->length - 12.0)));
}

void SoakDriver::op_assembly_drop_on_road() {
  // Drop a T/X assembly ONTO an existing road (gate finding 1): the on-road
  // attach must align + connect in one command, never superimpose a floating
  // junction. Refusals (target too short, drop near an end, target already in a
  // junction, paramPoly3 at the cut) are data — the invariants after are what
  // must hold.
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId target = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(target);
  if (road == nullptr || road->length < 40.0) {
    return;
  }
  const double s = rand_range(15.0, road->length - 15.0);
  if (chance(0.5)) {
    push(edit::assembly::tee_onto_road(document_.network(), target, s));
  } else {
    push(edit::assembly::cross_onto_road(document_.network(), target, s));
  }
}

void SoakDriver::op_extend_road() {
  // Keep drawing off a road's END (Create Road extend). Refusals (a linked or
  // unauthored end, a point behind) are data.
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr) {
    return;
  }
  const PathPoint end = road->plan_view.evaluate(road->plan_view.length());
  const double reach = rand_range(20.0, 80.0);
  const double heading = end.hdg + rand_range(-0.5, 0.5); // mostly forward, some off-axis
  const Waypoint to{.x = end.x + (reach * std::cos(heading)),
                    .y = end.y + (reach * std::sin(heading))};
  push(edit::extend_road(
      document_.network(), RoadEnd{.road = road_id, .contact = ContactPoint::End}, to));
}

void SoakDriver::op_cross_commit() {
  // Author a new road straight across a target's interior — the Create Road
  // draw-across commit (create + cross_roads). Refusals are data.
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId target = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(target);
  if (road == nullptr || road->length < 40.0) {
    return;
  }
  const double s = rand_range(15.0, road->length - 15.0);
  const PathPoint pose = road->plan_view.evaluate(s);
  const double perp = pose.hdg + (std::numbers::pi / 2.0);
  const double reach = rand_range(30.0, 60.0);
  std::vector<Waypoint> waypoints{
      Waypoint{.x = pose.x - (reach * std::cos(perp)), .y = pose.y - (reach * std::sin(perp))},
      Waypoint{.x = pose.x + (reach * std::cos(perp)), .y = pose.y + (reach * std::sin(perp))}};
  push(edit::create_crossing_road(
      document_.network(), std::move(waypoints), random_profile(), {}, target));
}

void SoakDriver::op_tee_commit() {
  // Author a new stem onto a target's side — the Create Road side-snap commit
  // (create + attach_t_junction). Refusals are data.
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId target = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(target);
  if (road == nullptr || road->length < 40.0) {
    return;
  }
  const double s = rand_range(15.0, road->length - 15.0);
  const PathPoint pose = road->plan_view.evaluate(s);
  const double perp = pose.hdg + (std::numbers::pi / 2.0);
  const double reach = rand_range(30.0, 60.0);
  std::vector<Waypoint> stem{
      Waypoint{.x = pose.x, .y = pose.y},
      Waypoint{.x = pose.x + (reach * std::cos(perp)), .y = pose.y + (reach * std::sin(perp))}};
  push(edit::create_teed_road(
      document_.network(), std::move(stem), random_profile(), {}, target, s, ContactPoint::Start));
}

void SoakDriver::op_remove_lane() {
  // The discoverable lane-removal path (gate finding 6): remove the OUTERMOST
  // non-center lane of one side of a random section — what the per-side
  // properties-panel button and the context-menu item do. Kernel remove_lane
  // is stale-safe; a section down to just the driving lane refuses (recorded).
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return;
  }
  const LaneSectionId section_id =
      road->sections[static_cast<std::size_t>(rand_int(0, int(road->sections.size()) - 1))];
  const LaneSection* section = document_.network().lane_section(section_id);
  if (section == nullptr) {
    return;
  }
  // Pick the outermost lane on a chosen side (largest |odr_id|); skip center.
  const int side = chance(0.5) ? 1 : -1;
  LaneId outermost;
  int outermost_abs = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = document_.network().lane(lane_id);
    if (lane == nullptr || lane->odr_id == 0 || (lane->odr_id > 0) != (side > 0)) {
      continue;
    }
    if (std::abs(lane->odr_id) > outermost_abs) {
      outermost_abs = std::abs(lane->odr_id);
      outermost = lane_id;
    }
  }
  if (!outermost.is_valid()) {
    return;
  }
  push(edit::remove_lane(document_.network(), outermost));
}

void SoakDriver::op_insert_lane() {
  // Interior insert with renumbering — the path Lane Form/Carve build on. Picks
  // a random existing non-center lane as the insert point so the renumbering,
  // adjacent-section link remap, and junction lane_link remap all run; the
  // kernel refuses the center and any empty position (recorded, not fatal).
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->sections.empty()) {
    return;
  }
  const LaneSectionId section_id =
      road->sections[static_cast<std::size_t>(rand_int(0, int(road->sections.size()) - 1))];
  const LaneSection* section = document_.network().lane_section(section_id);
  if (section == nullptr || section->lanes.empty()) {
    return;
  }
  const LaneId at =
      section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
  const Lane* lane = document_.network().lane(at);
  if (lane == nullptr) {
    return;
  }
  const LaneType type = chance(0.5) ? LaneType::Driving : LaneType::Shoulder;
  push(edit::insert_lane(document_.network(), section_id, lane->odr_id, type));
}

/// Lane Add: a pocket lane over a random span. Deliberately does not clamp the
/// span itself — the kernel clamps inward, and a degenerate span is refused and
/// recorded (never fatal), matching op_split_lane_section.
void SoakDriver::op_lane_add_span() {
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->length <= 0.0) {
    return;
  }
  const double a = rand_range(-1.0, road->length + 1.0);
  const double b = rand_range(-1.0, road->length + 1.0);
  const int side = chance(0.5) ? 1 : -1;
  push(edit::add_lane_span(
      document_.network(), road_id, side, std::min(a, b), std::max(a, b), LaneType::Driving));
}

/// Lane Form: an interior lane from a random station to the road end. Picks an
/// existing lane's numbering position so the sign matches; the kernel refuses a
/// station that is not in the final section (recorded, not fatal).
void SoakDriver::op_lane_form() {
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->sections.empty() || road->length <= 0.0) {
    return;
  }
  const LaneSectionId last = road->sections.back();
  const LaneSection* section = document_.network().lane_section(last);
  if (section == nullptr || section->lanes.empty()) {
    return;
  }
  const LaneId at =
      section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
  const Lane* lane = document_.network().lane(at);
  if (lane == nullptr || lane->odr_id == 0) {
    return; // the centre lane has no side; try again next step
  }
  const int side = lane->odr_id > 0 ? 1 : -1;
  push(edit::form_lane(document_.network(),
                       road_id,
                       side,
                       rand_range(-1.0, road->length + 1.0),
                       lane->odr_id,
                       LaneType::Driving));
}

void SoakDriver::op_ground_surface() {
  // Author a closed square loop as four straight roads welded corner-to-corner
  // (the derive_surfaces "one enclosed area = one Surface" path). No command
  // sets a surface flag: each create_road is a topology change, so the
  // Document's after_kernel_mutation hook re-derives the surface arena; once
  // the fourth edge closes the ring a Surface appears, and the round-trip and
  // save/reload invariants then exercise its rm:surface userData markers.
  // Each push is independently a valid-ish command — a refusal just leaves the
  // loop open (no surface), which the invariants still hold for.
  const double ox = rand_range(-400.0, 400.0);
  const double oy = rand_range(-400.0, 400.0);
  const double side = rand_range(20.0, 60.0); // corners stay exactly coincident
  const std::array<Waypoint, 4> corners{Waypoint{.x = ox, .y = oy},
                                        Waypoint{.x = ox + side, .y = oy},
                                        Waypoint{.x = ox + side, .y = oy + side},
                                        Waypoint{.x = ox, .y = oy + side}};
  for (std::size_t i = 0; i < corners.size(); ++i) {
    std::vector<Waypoint> edge{corners[i], corners[(i + 1) % corners.size()]};
    push(edit::create_road(std::move(edge), random_profile(), {}));
  }
}

void SoakDriver::op_overpass() {
  // Apply the overpass elevation profile where a road crosses another (gate
  // finding 4): the SAME headless path ProfilePanel::apply_overpass drives. It
  // is pure elevation — check_invariants confirms it creates no junction and the
  // network still validates.
  const std::vector<RoadId> roads = live_roads(/*editable_only=*/true);
  if (roads.empty()) {
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  const std::vector<elevation::Crossing> crossings =
      elevation::find_crossings(document_.network(), road_id);
  if (crossings.empty()) {
    return;
  }
  std::vector<edit::ElevationPoint> points =
      elevation::overpass_points(document_.network(), road_id, chance(0.5));
  if (points.empty()) {
    return;
  }
  push(edit::set_elevation_profile(document_.network(), road_id, std::move(points)));
}

void SoakDriver::op_delete_crossing_road() {
  // Delete a road that is CROSSED by another (gate finding 4 integrity): the
  // survivor's geometry/profile must stay intact and the network keep
  // validating (both asserted by check_invariants). Overlaps op_delete_road but
  // targets the crossing case deterministically.
  for (const RoadId road_id : live_roads(/*editable_only=*/true)) {
    if (!elevation::find_crossings(document_.network(), road_id).empty()) {
      push(edit::delete_road(document_.network(), road_id));
      return;
    }
  }
}

void SoakDriver::op_delete_junction() {
  const std::vector<JunctionId> junctions = live_junctions();
  if (junctions.empty()) {
    return;
  }
  push(edit::delete_junction(
      document_.network(),
      junctions[static_cast<std::size_t>(rand_int(0, int(junctions.size()) - 1))]));
}

void SoakDriver::op_delete_road() {
  const std::vector<RoadId> roads = live_roads(true);
  if (roads.empty()) {
    return;
  }
  push(edit::delete_road(document_.network(),
                         roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))]));
}

void SoakDriver::op_undo_redo() {
  QUndoStack* stack = document_.undo_stack();
  const int undos = rand_int(1, 8);
  int done = 0;
  for (int i = 0; i < undos && stack->canUndo(); ++i) {
    stack->undo();
    ++stats_.undos;
    ++done;
  }
  const int redos = rand_int(0, done);
  for (int i = 0; i < redos && stack->canRedo(); ++i) {
    stack->redo();
    ++stats_.redos;
  }
}

void SoakDriver::op_save_reload() {
  if (document_.network().road_count() == 0) {
    return; // an empty document round-trips trivially; keep the op meaningful
  }
  const std::filesystem::path path = options_.work_dir / "soak.xodr";
  if (!document_.save(path).has_value()) {
    // save() only fails when the writer refuses the network — after a clean
    // validate that is a finding, and check_invariants would also trip.
    ++stats_.rejected;
    return;
  }
  const auto before = write_xodr(document_.network(), "soak");
  if (!document_.load(path).has_value()) {
    ++stats_.rejected;
    return;
  }
  ++stats_.saves;
  const auto after = write_xodr(document_.network(), "soak");
  if (before.has_value() && after.has_value() && *before != *after) {
    // Recorded as a failure through the shared invariant path on this op.
    failure_ = fmt::format("soak failure: seed={} save/reload not byte-identical", options_.seed);
  }
}

void SoakDriver::op_select() {
  const std::vector<RoadId> roads = live_roads(false);
  if (roads.empty()) {
    selection_.clear();
    return;
  }
  const RoadId road_id = roads[static_cast<std::size_t>(rand_int(0, int(roads.size()) - 1))];
  SelectionEntry entry{.road = road_id, .lane = {}};
  if (chance(0.4)) {
    const Road* road = document_.network().road(road_id);
    if (!road->sections.empty()) {
      const LaneSection* section = document_.network().lane_section(road->sections.front());
      if (section != nullptr && !section->lanes.empty()) {
        entry.lane =
            section->lanes[static_cast<std::size_t>(rand_int(0, int(section->lanes.size()) - 1))];
      }
    }
  }
  selection_.select(entry, chance(0.3) ? SelectMode::Toggle : SelectMode::Replace);
}

// --- helpers -----------------------------------------------------------------

double SoakDriver::rand_range(double lo, double hi) {
  return std::uniform_real_distribution<double>(lo, hi)(rng_);
}

int SoakDriver::rand_int(int lo, int hi) {
  return std::uniform_int_distribution<int>(lo, hi)(rng_);
}

bool SoakDriver::chance(double probability) {
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng_) < probability;
}

std::vector<RoadId> SoakDriver::live_roads(bool editable_only) const {
  std::vector<RoadId> roads;
  document_.network().for_each_road([&](RoadId road_id, const Road& road) {
    if (editable_only && road.junction.is_valid()) {
      return;
    }
    roads.push_back(road_id);
  });
  return roads;
}

std::vector<JunctionId> SoakDriver::live_junctions() const {
  std::vector<JunctionId> junctions;
  document_.network().for_each_junction(
      [&](JunctionId junction_id, const Junction&) { junctions.push_back(junction_id); });
  return junctions;
}

LaneProfile SoakDriver::random_profile() {
  switch (rand_int(0, 2)) {
  case 0:
    return LaneProfile::two_lane_rural();
  case 1:
    return LaneProfile::urban_sidewalk();
  default:
    return LaneProfile::highway();
  }
}

} // namespace roadmaker::editor::soak
