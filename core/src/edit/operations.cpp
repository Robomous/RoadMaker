#include "roadmaker/edit/operations.hpp"

#include "roadmaker/geometry/profile_fit.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <utility>
#include <variant>

namespace roadmaker::edit {

namespace {

// ---- generic command engine -------------------------------------------------
//
// Every edit operation decomposes into three ingredient sets:
//   - value edits: objects that exist before and after (before/after values),
//   - erasures:    objects the command deletes (values captured up front),
//   - creations:   objects the command brings into being on first apply
//                  (ids + values captured right after that apply).
// apply/revert over these sets is uniform, which is what makes the
// byte-equality round-trip contract easy to honor per operation.

struct Values {
  std::vector<std::pair<RoadId, Road>> roads;
  std::vector<std::pair<LaneSectionId, LaneSection>> sections;
  std::vector<std::pair<LaneId, Lane>> lanes;
  std::vector<std::pair<JunctionId, Junction>> junctions;
};

Expected<void> ensure_live(const RoadNetwork& network, const Values& values) {
  for (const auto& [id, value] : values.roads) {
    if (network.road(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale road id");
    }
  }
  for (const auto& [id, value] : values.sections) {
    if (network.lane_section(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale lane-section id");
    }
  }
  for (const auto& [id, value] : values.lanes) {
    if (network.lane(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale lane id");
    }
  }
  for (const auto& [id, value] : values.junctions) {
    if (network.junction(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale junction id");
    }
  }
  return {};
}

/// Callers ensure_live first; assignment cannot fail afterwards.
void write_values(RoadNetwork& network, const Values& values) {
  for (const auto& [id, value] : values.roads) {
    *network.road(id) = value;
  }
  for (const auto& [id, value] : values.sections) {
    *network.lane_section(id) = value;
  }
  for (const auto& [id, value] : values.lanes) {
    *network.lane(id) = value;
  }
  for (const auto& [id, value] : values.junctions) {
    *network.junction(id) = value;
  }
}

/// Same ids as `ids`, values re-read from the network (post-mutation).
Values read_values(const RoadNetwork& network, const Values& ids) {
  Values out;
  for (const auto& [id, value] : ids.roads) {
    out.roads.emplace_back(id, *network.road(id));
  }
  for (const auto& [id, value] : ids.sections) {
    out.sections.emplace_back(id, *network.lane_section(id));
  }
  for (const auto& [id, value] : ids.lanes) {
    out.lanes.emplace_back(id, *network.lane(id));
  }
  for (const auto& [id, value] : ids.junctions) {
    out.junctions.emplace_back(id, *network.junction(id));
  }
  return out;
}

Expected<void> restore_values(RoadNetwork& network, const Values& values) {
  for (const auto& [id, value] : values.roads) {
    if (auto restored = network.restore_road(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  for (const auto& [id, value] : values.sections) {
    if (auto restored = network.restore_lane_section(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  for (const auto& [id, value] : values.lanes) {
    if (auto restored = network.restore_lane(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  for (const auto& [id, value] : values.junctions) {
    if (auto restored = network.restore_junction(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  return {};
}

Expected<void> erase_values_exact(RoadNetwork& network, const Values& values) {
  // Leaf to root, so a partially-visible intermediate state never has a
  // parent without its children.
  for (const auto& [id, value] : values.lanes) {
    if (auto erased = network.erase_lane_exact(id); !erased.has_value()) {
      return erased;
    }
  }
  for (const auto& [id, value] : values.sections) {
    if (auto erased = network.erase_lane_section_exact(id); !erased.has_value()) {
      return erased;
    }
  }
  for (const auto& [id, value] : values.roads) {
    if (auto erased = network.erase_road_exact(id); !erased.has_value()) {
      return erased;
    }
  }
  for (const auto& [id, value] : values.junctions) {
    if (auto erased = network.erase_junction_exact(id); !erased.has_value()) {
      return erased;
    }
  }
  return {};
}

/// First-apply hook for commands that create objects. Must either mutate the
/// network fully (registering every created id in `created`) or fail without
/// mutating at all — the validate-first-mutate-after contract.
using Creator = std::function<Expected<void>(RoadNetwork&, Values& created)>;

class GenericCommand final : public Command {
public:
  GenericCommand(std::string name, DirtySet dirty)
      : name_(std::move(name)), dirty_(std::move(dirty)) {}

  // Factory-time configuration (see file-head comment).
  Values before;
  Values after; // precomputed; when `creator` is set it is re-read post-run
  Values erased;
  Creator creator;
  std::optional<Error> invalid; // factory-time validation failure

  Expected<void> apply(RoadNetwork& network) override {
    if (invalid.has_value()) {
      return tl::unexpected<Error>(*invalid);
    }
    if (auto live = ensure_live(network, before); !live.has_value()) {
      return live;
    }
    if (auto live = ensure_live(network, erased); !live.has_value()) {
      return live;
    }
    if (!applied_once_) {
      if (creator) {
        if (auto ran = creator(network, created_); !ran.has_value()) {
          return ran;
        }
        created_ = read_values(network, created_);
        after = read_values(network, before);
        for (const auto& [id, value] : created_.roads) {
          dirty_.roads.push_back(id);
        }
        for (const auto& [id, value] : created_.junctions) {
          dirty_.junctions.push_back(id);
        }
      } else {
        write_values(network, after);
      }
      applied_once_ = true;
      return erase_values_exact(network, erased);
    }
    if (auto restored = restore_values(network, created_); !restored.has_value()) {
      return restored;
    }
    write_values(network, after);
    return erase_values_exact(network, erased);
  }

  Expected<void> revert(RoadNetwork& network) override {
    if (auto restored = restore_values(network, erased); !restored.has_value()) {
      return restored;
    }
    if (auto live = ensure_live(network, before); !live.has_value()) {
      return live;
    }
    write_values(network, before);
    return erase_values_exact(network, created_);
  }

  std::string_view name() const override { return name_; }

  DirtySet dirty() const override { return dirty_; }

private:
  bool applied_once_ = false;
  Values created_;
  std::string name_;
  DirtySet dirty_;
};

std::unique_ptr<Command> invalid_command(std::string name, Error error) {
  auto command = std::make_unique<GenericCommand>(std::move(name), DirtySet{});
  command->invalid = std::move(error);
  return command;
}

/// A command composed of child commands applied in order and reverted in
/// reverse. Children AFTER the first may be built lazily during the first
/// apply (their builder sees the network with the previous children already
/// applied) — how attach_t_junction learns the arena ids its later stages
/// need, since those are assigned at apply time. A failing child unwinds the
/// already-applied prefix, so a failed apply leaves the network untouched;
/// redo re-applies the captured children, which resurrect their created
/// objects under the original ids (the restore-in-place contract).
class CompositeCommand final : public Command {
public:
  using Builder = std::function<std::unique_ptr<Command>(RoadNetwork&)>;

  CompositeCommand(std::string name, DirtySet base_dirty, std::vector<Builder> builders)
      : name_(std::move(name)), base_dirty_(std::move(base_dirty)), builders_(std::move(builders)) {
    children_.resize(builders_.size());
  }

  Expected<void> apply(RoadNetwork& network) override {
    for (std::size_t i = 0; i < builders_.size(); ++i) {
      if (children_[i] == nullptr) {
        children_[i] = builders_[i](network);
      }
      Expected<void> applied =
          children_[i] != nullptr
              ? children_[i]->apply(network)
              : Expected<void>(
                    make_error(ErrorCode::InvalidArgument, "composite stage yielded no command"));
      if (!applied.has_value()) {
        // Unwind the applied prefix — the whole composite is atomic.
        for (std::size_t k = i; k-- > 0;) {
          (void)children_[k]->revert(network);
        }
        // A lazily-built child that failed its own apply captured state from
        // a network the unwind just rewound — drop it so a redo rebuilds it
        // against the (identical) re-applied prefix.
        if (children_[i] != nullptr) {
          children_[i] = nullptr;
        }
        return applied;
      }
    }
    return {};
  }

  Expected<void> revert(RoadNetwork& network) override {
    for (std::size_t i = children_.size(); i-- > 0;) {
      if (children_[i] == nullptr) {
        continue; // never applied (failed composite apply)
      }
      if (auto reverted = children_[i]->revert(network); !reverted.has_value()) {
        return reverted;
      }
    }
    return {};
  }

  std::string_view name() const override { return name_; }

  DirtySet dirty() const override {
    DirtySet dirty = base_dirty_;
    for (const auto& child : children_) {
      if (child == nullptr) {
        continue;
      }
      const DirtySet child_dirty = child->dirty();
      for (const RoadId road : child_dirty.roads) {
        if (std::ranges::find(dirty.roads, road) == dirty.roads.end()) {
          dirty.roads.push_back(road);
        }
      }
      for (const JunctionId junction : child_dirty.junctions) {
        if (std::ranges::find(dirty.junctions, junction) == dirty.junctions.end()) {
          dirty.junctions.push_back(junction);
        }
      }
      dirty.topology = dirty.topology || child_dirty.topology;
    }
    return dirty;
  }

private:
  std::string name_;
  DirtySet base_dirty_;
  std::vector<Builder> builders_;
  std::vector<std::unique_ptr<Command>> children_;
};

// ---- shared lookups and geometry helpers -------------------------------------

struct LaneContext {
  Lane lane;
  LaneId lane_id;
  LaneSectionId section_id;
  RoadId road_id;
};

Expected<LaneContext> lane_context(const RoadNetwork& network, LaneId lane_id) {
  const Lane* lane = network.lane(lane_id);
  if (lane == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale lane id");
  }
  const LaneSection* section = network.lane_section(lane->section);
  if (section == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "lane has a stale section back-reference");
  }
  return LaneContext{
      .lane = *lane, .lane_id = lane_id, .section_id = lane->section, .road_id = section->road};
}

std::string next_free_road_odr_id(const RoadNetwork& network) {
  int candidate = 1;
  while (network.find_road(std::to_string(candidate)).is_valid()) {
    ++candidate;
  }
  return std::to_string(candidate);
}

std::string next_free_junction_odr_id(const RoadNetwork& network) {
  int candidate = 1;
  while (network.find_junction(std::to_string(candidate)).is_valid()) {
    ++candidate;
  }
  return std::to_string(candidate);
}

/// §2.5: waypoints for a road that has none recorded — every geometry-record
/// start plus the final endpoint.
std::vector<Waypoint> derive_waypoints(const Road& road) {
  std::vector<Waypoint> waypoints;
  waypoints.reserve(road.plan_view.records().size() + 1);
  for (const GeometryRecord& record : road.plan_view.records()) {
    waypoints.push_back(Waypoint{.x = record.x, .y = record.y});
  }
  const PathPoint end = road.plan_view.evaluate(road.plan_view.length());
  waypoints.push_back(Waypoint{.x = end.x, .y = end.y});
  return waypoints;
}

/// Stations of the waypoints along the CURRENT reference line: record starts
/// plus the total length. Only meaningful while waypoints and records are in
/// sync (waypoint count == record count + 1, the fit invariant).
Expected<std::vector<double>> waypoint_stations(const Road& road, std::size_t waypoint_count) {
  const auto& records = road.plan_view.records();
  if (waypoint_count != records.size() + 1) {
    return make_error(ErrorCode::InvalidArgument,
                      "authoring waypoints are out of sync with the plan-view geometry");
  }
  std::vector<double> stations;
  stations.reserve(waypoint_count);
  for (const GeometryRecord& record : records) {
    stations.push_back(record.s);
  }
  stations.push_back(road.plan_view.length());
  return stations;
}

/// Re-expresses `poly` about a new origin `origin` >= poly.s (Taylor shift);
/// the returned record starts at s = 0.
Poly3 shift_to_origin(const Poly3& poly, double origin) {
  const double delta = origin - poly.s;
  return Poly3{
      .s = 0.0,
      .a = poly.a + (poly.b * delta) + (poly.c * delta * delta) + (poly.d * delta * delta * delta),
      .b = poly.b + (2.0 * poly.c * delta) + (3.0 * poly.d * delta * delta),
      .c = poly.c + (3.0 * poly.d * delta),
      .d = poly.d,
  };
}

/// Keeps records starting before `split` (the head profile is unchanged —
/// evaluation never reads past the shorter road's length).
std::vector<Poly3> truncate_profile(std::span<const Poly3> profile, double split) {
  std::vector<Poly3> out;
  for (const Poly3& poly : profile) {
    if (poly.s < split - tol::kLength) {
      out.push_back(poly);
    }
  }
  return out;
}

/// Rebases the tail of a profile onto a coordinate system starting at
/// `split`: the record covering the split is Taylor-shifted to s = 0,
/// records after it keep their coefficients with s reduced.
std::vector<Poly3> rebase_profile(std::span<const Poly3> profile, double split) {
  std::vector<Poly3> out;
  const Poly3* covering = nullptr;
  for (const Poly3& poly : profile) {
    if (poly.s <= split + tol::kLength) {
      covering = &poly;
    } else {
      out.push_back(Poly3{.s = poly.s - split, .a = poly.a, .b = poly.b, .c = poly.c, .d = poly.d});
    }
  }
  if (covering != nullptr) {
    out.insert(out.begin(), shift_to_origin(*covering, split));
  }
  return out;
}

/// Head part of a geometry record cut at local arc length ds (0 < ds < len).
GeometryRecord record_head(const GeometryRecord& record, double ds) {
  GeometryRecord head = record;
  head.length = ds;
  if (const auto* spiral = std::get_if<SpiralGeom>(&record.shape)) {
    const double rate = (spiral->curv_end - spiral->curv_start) / record.length;
    head.shape =
        SpiralGeom{.curv_start = spiral->curv_start, .curv_end = spiral->curv_start + rate * ds};
  }
  return head;
}

/// Tail part; the start pose comes from evaluating the owning line.
GeometryRecord record_tail(const GeometryRecord& record, double ds, const PathPoint& start) {
  GeometryRecord tail{.x = start.x, .y = start.y, .hdg = start.hdg, .length = record.length - ds};
  if (std::holds_alternative<LineGeom>(record.shape)) {
    tail.shape = LineGeom{};
  } else if (const auto* arc = std::get_if<ArcGeom>(&record.shape)) {
    tail.shape = *arc;
  } else {
    const auto& spiral = std::get<SpiralGeom>(record.shape);
    const double rate = (spiral.curv_end - spiral.curv_start) / record.length;
    tail.shape =
        SpiralGeom{.curv_start = spiral.curv_start + rate * ds, .curv_end = spiral.curv_end};
  }
  return tail;
}

/// Does `link` point at this road?
bool links_to_road(const std::optional<RoadLink>& link, RoadId road) {
  if (!link.has_value()) {
    return false;
  }
  const auto* target = std::get_if<RoadId>(&link->target);
  return target != nullptr && *target == road;
}

// ---- deletion closure (docs/design/m2/02_editing_tools.md §7) -----------------

bool contains_road(std::span<const RoadId> roads, RoadId id) {
  return std::ranges::find(roads, id) != roads.end();
}

/// Does `link` point at any doomed road, or at the doomed junction?
bool links_into_closure(const std::optional<RoadLink>& link,
                        std::span<const RoadId> doomed,
                        std::optional<JunctionId> doomed_junction) {
  if (!link.has_value()) {
    return false;
  }
  if (const auto* road = std::get_if<RoadId>(&link->target)) {
    return contains_road(doomed, *road);
  }
  return doomed_junction.has_value() && std::get<JunctionId>(link->target) == *doomed_junction;
}

/// Expands seed roads to the full deletion closure: a junction connection
/// whose INCOMING road dies takes its connecting road along (the turn cannot
/// outlive the road it comes from). Runs to a fixpoint so chains resolve even
/// on data violating
/// asam.net:xodr:1.4.0:junctions.connection.connect_road_no_incoming_road
/// (a connecting road acting as an incoming road elsewhere).
std::vector<RoadId> deletion_closure(const RoadNetwork& network, std::vector<RoadId> doomed) {
  bool grew = true;
  while (grew) {
    grew = false;
    network.for_each_junction([&](JunctionId, const Junction& junction) {
      for (const JunctionConnection& connection : junction.connections) {
        if (contains_road(doomed, connection.incoming_road) &&
            !contains_road(doomed, connection.connecting_road) &&
            network.road(connection.connecting_road) != nullptr) {
          doomed.push_back(connection.connecting_road);
          grew = true;
        }
      }
    });
  }
  return doomed;
}

/// Captures onto `command` everything the closure deletion touches: the
/// doomed roads with their sections and lanes (plus `doomed_junction` when
/// set) as erasures, and — as before/after value edits — surviving junctions
/// whose connections reference a doomed road, and surviving roads whose
/// links (or junction back-reference) point into the deleted set. Undo
/// restores every removed object and link exactly.
void capture_deletion(const RoadNetwork& network,
                      GenericCommand& command,
                      std::span<const RoadId> doomed,
                      std::optional<JunctionId> doomed_junction) {
  for (const RoadId road_id : doomed) {
    const Road* road = network.road(road_id);
    command.erased.roads.emplace_back(road_id, *road);
    for (const LaneSectionId section_id : road->sections) {
      const LaneSection* section = network.lane_section(section_id);
      command.erased.sections.emplace_back(section_id, *section);
      for (const LaneId lane_id : section->lanes) {
        command.erased.lanes.emplace_back(lane_id, *network.lane(lane_id));
      }
    }
  }
  if (doomed_junction.has_value()) {
    command.erased.junctions.emplace_back(*doomed_junction, *network.junction(*doomed_junction));
  }

  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    if (doomed_junction.has_value() && junction_id == *doomed_junction) {
      return; // erased wholesale, connections and all
    }
    const auto doomed_connection = [&doomed](const JunctionConnection& connection) {
      return contains_road(doomed, connection.incoming_road) ||
             contains_road(doomed, connection.connecting_road);
    };
    // Arms are part of the closure too: regeneration re-runs from the
    // recorded arm list, so an arm surviving its road is a dangling id the
    // next regeneration (or any arm walker) trips over — found by the soak
    // driver as issue #89. An arm can be doomed without any connection
    // being doomed (its turns were all dropped at generation), so the gate
    // checks both.
    const auto doomed_arm = [&doomed](const RoadEnd& arm) {
      return contains_road(doomed, arm.road);
    };
    if (!std::ranges::any_of(junction.connections, doomed_connection) &&
        !std::ranges::any_of(junction.arms, doomed_arm)) {
      return;
    }
    Junction after = junction;
    std::erase_if(after.connections, doomed_connection);
    std::erase_if(after.arms, doomed_arm);
    command.before.junctions.emplace_back(junction_id, junction);
    command.after.junctions.emplace_back(junction_id, std::move(after));
  });

  network.for_each_road([&](RoadId road_id, const Road& road) {
    if (contains_road(doomed, road_id)) {
      return;
    }
    const bool backref = doomed_junction.has_value() && road.junction == *doomed_junction;
    if (!backref && !links_into_closure(road.predecessor, doomed, doomed_junction) &&
        !links_into_closure(road.successor, doomed, doomed_junction)) {
      return;
    }
    Road after = road;
    if (backref) {
      after.junction = {};
    }
    if (links_into_closure(after.predecessor, doomed, doomed_junction)) {
      after.predecessor.reset();
    }
    if (links_into_closure(after.successor, doomed, doomed_junction)) {
      after.successor.reset();
    }
    command.before.roads.emplace_back(road_id, road);
    command.after.roads.emplace_back(road_id, std::move(after));
  });
}

// ---- waypoint re-fit commands -------------------------------------------------

/// Headings of the current plan view at the derive_waypoints() stations
/// (record starts + endpoint). The §2.5 derivation re-fit interpolates
/// these (G1 Hermite), so the first edit of a foreign road reproduces every
/// untouched line/arc/spiral segment exactly — a points-only re-fit would
/// reflow the whole chain by up to ~1 m. Later edits (waypoints recorded)
/// re-fit through positions alone, the authored-road reflow semantics.
std::vector<double> derived_headings(const Road& road) {
  std::vector<double> headings;
  headings.reserve(road.plan_view.records().size() + 1);
  for (const GeometryRecord& record : road.plan_view.records()) {
    headings.push_back(record.hdg);
  }
  headings.push_back(road.plan_view.evaluate(road.plan_view.length()).hdg);
  return headings;
}

/// Interactive-fit loop guard (issue #93, maintainer CRASH-1): a sharp
/// turn-back between waypoints makes the G1 spline loop, and the fitted
/// length can grow without bound — everything downstream (mesh sampling,
/// road-mark quads) scales with it until the editor dies. Mirror of the
/// junction generator's max_loop_factor (k=4, 02 §6), applied only on the
/// AUTHORING paths — derivation re-fits of foreign geometry may be
/// legitimately loopy and are not gated here.
constexpr double kMaxFitLoopFactor = 4.0;

Expected<void> check_fit_bounded(const ReferenceLine& line, std::span<const Waypoint> waypoints) {
  double polyline = 0.0;
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    polyline +=
        std::hypot(waypoints[i].x - waypoints[i - 1].x, waypoints[i].y - waypoints[i - 1].y);
  }
  if (line.length() > kMaxFitLoopFactor * std::max(polyline, tol::kLength)) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("fitted road loops ({:.0f} m for a {:.0f} m waypoint span) — "
                                  "soften the turn or add intermediate waypoints",
                                  line.length(),
                                  polyline));
  }
  return {};
}

std::unique_ptr<Command> refit_command(const RoadNetwork& network,
                                       RoadId road_id,
                                       std::string command_name,
                                       std::vector<Waypoint> waypoints,
                                       std::span<const double> headings = {}) {
  const Road* road = network.road(road_id);
  auto line =
      headings.empty() ? fit_clothoid_path(waypoints) : fit_clothoid_path(waypoints, headings);
  if (!line.has_value()) {
    return invalid_command(std::move(command_name), line.error());
  }
  if (headings.empty()) {
    // Authored (points-only) re-fit: gate runaway loops (#93). The headings
    // overload is the §2.5 derivation re-fit, which must keep reproducing
    // foreign geometry however loopy it legitimately is.
    if (auto bounded = check_fit_bounded(*line, waypoints); !bounded.has_value()) {
      return invalid_command(std::move(command_name), bounded.error());
    }
  }
  const double new_length = line->length();
  for (const LaneSectionId section_id : road->sections) {
    if (network.lane_section(section_id)->s0 >= new_length - tol::kLength) {
      return invalid_command(std::move(command_name),
                             Error{.code = ErrorCode::InvalidArgument,
                                   .message = "edit would move a lane section past the road end",
                                   .context = road->odr_id});
    }
  }

  Road after = *road;
  after.plan_view = std::move(*line);
  after.length = after.plan_view.length();
  after.authoring_waypoints = std::move(waypoints);

  auto command = std::make_unique<GenericCommand>(
      std::move(command_name),
      DirtySet{.roads = {road_id}, .junctions = junctions_touching(network, road_id)});
  command->before.roads.emplace_back(road_id, *road);
  command->after.roads.emplace_back(road_id, std::move(after));
  return command;
}

// ---- junction connecting-road generator (docs/design/m2/02 §6) ----------------

/// A driving lane at an arm's junction-facing end, with its width there [m].
struct ArmLane {
  int odr_id = 0;
  double width = 0.0;
};

/// The junction-facing end of an arm: its point, the tangent leaving the arm
/// INTO the junction (a connecting road's start heading here) and the tangent
/// entering the arm OUT of the junction (a connecting road's end heading
/// here), plus the lane section and station at that end. All angles [rad] in
/// the inertial frame.
struct ArmEnd {
  double x = 0.0;
  double y = 0.0;
  double into_hdg = 0.0;
  double out_hdg = 0.0;
  LaneSectionId section;
  double station = 0.0;
};

/// Width [m] of `lane` at road station `road_s` (widths are section-local,
/// evaluated on the last record starting at or before the station).
double lane_width_at(const Lane& lane, const LaneSection& section, double road_s) {
  if (lane.widths.empty()) {
    return 0.0;
  }
  const double local = road_s - section.s0;
  const Poly3* width = &lane.widths.front();
  for (const Poly3& poly : lane.widths) {
    if (poly.s <= local + tol::kLength) {
      width = &poly;
    }
  }
  const double ds = local - width->s;
  return width->a + (width->b * ds) + (width->c * ds * ds) + (width->d * ds * ds * ds);
}

Expected<ArmEnd> arm_end(const RoadNetwork& network, const RoadEnd& end) {
  const Road* road = network.road(end.road);
  if (road == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale road id in road ends");
  }
  if (road->sections.empty()) {
    return make_error(ErrorCode::InvalidArgument, "arm road has no lane sections", road->odr_id);
  }
  constexpr double kPi = std::numbers::pi;
  ArmEnd out;
  if (end.contact == ContactPoint::Start) {
    const PathPoint pose = road->plan_view.evaluate(0.0);
    out = ArmEnd{.x = pose.x,
                 .y = pose.y,
                 .into_hdg = pose.hdg + kPi, // travel toward decreasing s enters the junction
                 .out_hdg = pose.hdg,        // road body continues along +s
                 .section = road->sections.front(),
                 .station = 0.0};
  } else {
    const double station = road->plan_view.length();
    const PathPoint pose = road->plan_view.evaluate(station);
    out = ArmEnd{.x = pose.x,
                 .y = pose.y,
                 .into_hdg = pose.hdg,      // travel toward increasing s enters the junction
                 .out_hdg = pose.hdg + kPi, // road body continues along -s
                 .section = road->sections.back(),
                 .station = station};
  }
  return out;
}

/// Driving lanes at an arm's junction end, ordered curb-in (outermost first).
/// `incoming` picks the lanes leading INTO the junction; otherwise the lanes
/// leading OUT (which serve as the outgoing road, 12.3). Which sign leads in
/// depends on the contact end: at a Start end traffic toward decreasing s
/// (positive/left lanes) enters; at an End end traffic toward increasing s
/// (negative/right lanes) enters.
std::vector<ArmLane> arm_driving_lanes(const RoadNetwork& network,
                                       const RoadEnd& end,
                                       const ArmEnd& geom,
                                       bool incoming) {
  const LaneSection& section = *network.lane_section(geom.section);
  const bool positive_leads_in = end.contact == ContactPoint::Start;
  const bool want_positive = incoming ? positive_leads_in : !positive_leads_in;
  std::vector<ArmLane> lanes;
  for (const LaneId lane_id : section.lanes) { // leftmost first: +N..+1,0,-1..-N
    const Lane& lane = *network.lane(lane_id);
    if (lane.type != LaneType::Driving) {
      continue;
    }
    if (want_positive ? (lane.odr_id > 0) : (lane.odr_id < 0)) {
      lanes.push_back(
          ArmLane{.odr_id = lane.odr_id, .width = lane_width_at(lane, section, geom.station)});
    }
  }
  // Positive lanes already arrive outermost-first (+N..+1); negative lanes
  // arrive innermost-first (-1..-N), so reverse them to curb-in order.
  if (!want_positive) {
    std::ranges::reverse(lanes);
  }
  return lanes;
}

/// A connecting road the generator will build for one permitted turn.
struct ConnectingPlan {
  RoadEnd from;       // incoming arm
  RoadEnd to;         // outgoing arm
  int from_lane = 0;  // incoming lane odr id on `from`
  int to_lane = 0;    // outgoing lane odr id on `to`
  ReferenceLine line; // driving-direction reference line, from → to
  double start_width = 0.0;
  double end_width = 0.0;
};

struct JunctionPlan {
  std::vector<ConnectingPlan> roads;
  std::vector<std::string> dropped;
};

double end_distance(const ArmEnd& a, const ArmEnd& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

/// The deterministic core shared by preview/create/regenerate: enumerates the
/// connecting roads for every ordered arm pair (A_in → B_out, A ≠ B, no
/// U-turns), matching driving lanes curb-in. Turns whose G1 clothoid loops
/// (length > k·end-distance) are dropped with a note; ends farther apart than
/// the limit are a hard error. Does NOT check link-slot occupancy — that is a
/// create-only precondition (regeneration runs on already-linked arms).
Expected<JunctionPlan> plan_junction(const RoadNetwork& network,
                                     std::span<const RoadEnd> ends,
                                     const JunctionGenOptions& options) {
  if (ends.size() < 2) {
    return make_error(ErrorCode::InvalidArgument, "a junction needs at least 2 road ends");
  }
  for (std::size_t i = 0; i < ends.size(); ++i) {
    for (std::size_t j = i + 1; j < ends.size(); ++j) {
      if (ends[i] == ends[j]) {
        return make_error(ErrorCode::InvalidArgument, "duplicate road end");
      }
    }
  }
  std::vector<ArmEnd> arm_ends;
  arm_ends.reserve(ends.size());
  for (const RoadEnd& end : ends) {
    auto geom = arm_end(network, end);
    if (!geom.has_value()) {
      return tl::unexpected<Error>(geom.error());
    }
    arm_ends.push_back(*geom);
  }
  for (std::size_t i = 0; i < arm_ends.size(); ++i) {
    for (std::size_t j = i + 1; j < arm_ends.size(); ++j) {
      const double dist = end_distance(arm_ends[i], arm_ends[j]);
      if (dist > options.max_end_distance_m + tol::kLength) {
        return make_error(
            ErrorCode::InvalidArgument,
            fmt::format("road ends are {:.1f} m apart, exceeding the {:.1f} m junction limit",
                        dist,
                        options.max_end_distance_m));
      }
    }
  }

  JunctionPlan plan;
  for (std::size_t i = 0; i < ends.size(); ++i) {
    for (std::size_t j = 0; j < ends.size(); ++j) {
      if (i == j) {
        continue; // U-turns omitted in M2
      }
      const std::vector<ArmLane> incoming =
          arm_driving_lanes(network, ends[i], arm_ends[i], /*incoming=*/true);
      const std::vector<ArmLane> outgoing =
          arm_driving_lanes(network, ends[j], arm_ends[j], /*incoming=*/false);
      const std::size_t pairs = std::min(incoming.size(), outgoing.size());
      const double dist = end_distance(arm_ends[i], arm_ends[j]);
      for (std::size_t k = 0; k < pairs; ++k) {
        const std::array<Waypoint, 2> waypoints{Waypoint{arm_ends[i].x, arm_ends[i].y},
                                                Waypoint{arm_ends[j].x, arm_ends[j].y}};
        const std::array<double, 2> headings{arm_ends[i].into_hdg, arm_ends[j].out_hdg};
        const auto describe = [&] {
          return fmt::format("{}→{} (lane {}→{})",
                             network.road(ends[i].road)->odr_id,
                             network.road(ends[j].road)->odr_id,
                             incoming[k].odr_id,
                             outgoing[k].odr_id);
        };
        auto line = fit_clothoid_path(waypoints, headings);
        if (!line.has_value()) {
          plan.dropped.push_back(describe() + ": clothoid fit failed");
          continue;
        }
        if (line->length() > options.max_loop_factor * dist) {
          plan.dropped.push_back(describe() + ": fitted turn loops");
          continue;
        }
        plan.roads.push_back(ConnectingPlan{.from = ends[i],
                                            .to = ends[j],
                                            .from_lane = incoming[k].odr_id,
                                            .to_lane = outgoing[k].odr_id,
                                            .line = std::move(*line),
                                            .start_width = incoming[k].width,
                                            .end_width = outgoing[k].width});
      }
    }
  }
  return plan;
}

/// A connecting road's single-lane blended width profile: linear source →
/// target along its length (constant when the widths match).
Poly3 connecting_lane_width(const ConnectingPlan& plan, double length) {
  return Poly3{.s = 0.0,
               .a = plan.start_width,
               .b = length > tol::kLength ? (plan.end_width - plan.start_width) / length : 0.0};
}

/// create-only precondition: every incoming end's link slot must be free.
Expected<void> ends_link_slots_free(const RoadNetwork& network, std::span<const RoadEnd> ends) {
  for (const RoadEnd& end : ends) {
    const Road* road = network.road(end.road);
    if (road == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale road id in road ends");
    }
    const auto& slot = end.contact == ContactPoint::Start ? road->predecessor : road->successor;
    if (slot.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("road '{}' is already linked at that end", road->odr_id));
    }
  }
  return {};
}

} // namespace

std::vector<Waypoint> effective_waypoints(const Road& road) {
  return road.authoring_waypoints.has_value() ? *road.authoring_waypoints : derive_waypoints(road);
}

Expected<std::vector<double>> waypoint_stations(const Road& road) {
  return waypoint_stations(road, effective_waypoints(road).size());
}

std::unique_ptr<Command>
move_waypoint(const RoadNetwork& network, RoadId road_id, std::size_t index, Waypoint to) {
  static constexpr std::string_view kName = "Move Waypoint";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  std::vector<Waypoint> waypoints = effective_waypoints(*road);
  if (index >= waypoints.size()) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "waypoint index out of range"});
  }
  std::vector<double> headings;
  if (!road->authoring_waypoints.has_value()) {
    headings = derived_headings(*road); // the moved node keeps the chain's heading
  }
  waypoints[index] = to;
  return refit_command(network, road_id, std::string(kName), std::move(waypoints), headings);
}

std::unique_ptr<Command>
insert_waypoint(const RoadNetwork& network, RoadId road_id, std::size_t index, Waypoint at) {
  static constexpr std::string_view kName = "Insert Waypoint";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  std::vector<Waypoint> waypoints = effective_waypoints(*road);
  if (index > waypoints.size()) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "waypoint index out of range"});
  }
  std::vector<double> headings;
  if (!road->authoring_waypoints.has_value()) {
    headings = derived_headings(*road);
    // Heading for the new node: the chain's heading midway between its
    // neighbors' stations (exact for the on-curve midpoint-marker insert).
    // Derived waypoints always satisfy the count == records + 1 invariant,
    // so the station lookup cannot fail here.
    const auto stations = waypoint_stations(*road, waypoints.size());
    const double lo = index == 0 ? stations->front() : stations->at(index - 1);
    const double hi = index == waypoints.size() ? stations->back() : stations->at(index);
    headings.insert(headings.begin() + static_cast<std::ptrdiff_t>(index),
                    road->plan_view.evaluate((lo + hi) / 2.0).hdg);
  }
  waypoints.insert(waypoints.begin() + static_cast<std::ptrdiff_t>(index), at);
  return refit_command(network, road_id, std::string(kName), std::move(waypoints), headings);
}

std::unique_ptr<Command>
delete_waypoint(const RoadNetwork& network, RoadId road_id, std::size_t index) {
  static constexpr std::string_view kName = "Delete Waypoint";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  std::vector<Waypoint> waypoints = effective_waypoints(*road);
  if (index >= waypoints.size()) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "waypoint index out of range"});
  }
  if (waypoints.size() <= 2) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "a road needs at least 2 waypoints"});
  }
  std::vector<double> headings;
  if (!road->authoring_waypoints.has_value()) {
    headings = derived_headings(*road);
    headings.erase(headings.begin() + static_cast<std::ptrdiff_t>(index));
  }
  waypoints.erase(waypoints.begin() + static_cast<std::ptrdiff_t>(index));
  return refit_command(network, road_id, std::string(kName), std::move(waypoints), headings);
}

std::unique_ptr<Command> create_road(std::vector<Waypoint> waypoints,
                                     LaneProfile profile,
                                     std::string name,
                                     EndpointHeadings locked) {
  static constexpr std::string_view kName = "Create Road";
  // Pre-validate the fit so obviously-bad input fails at factory time; the
  // authoring call re-validates against the live network on apply.
  auto fit = fit_clothoid_path(waypoints, locked);
  if (!fit.has_value()) {
    return invalid_command(std::string(kName), fit.error());
  }
  if (auto bounded = check_fit_bounded(*fit, waypoints); !bounded.has_value()) {
    return invalid_command(std::string(kName), bounded.error());
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.topology = true});
  command->creator = [waypoints = std::move(waypoints),
                      profile = std::move(profile),
                      name = std::move(name),
                      locked](RoadNetwork& target, Values& created) -> Expected<void> {
    // author_clothoid_road validates everything before its first mutation.
    auto road_id = author_clothoid_road(target, waypoints, profile, name, {}, locked);
    if (!road_id.has_value()) {
      return tl::unexpected<Error>(road_id.error());
    }
    if (name.empty()) {
      Road& road = *target.road(*road_id);
      road.name = "Road " + road.odr_id;
    }
    created.roads.emplace_back(*road_id, Road{});
    for (const LaneSectionId section_id : target.road(*road_id)->sections) {
      created.sections.emplace_back(section_id, LaneSection{});
      for (const LaneId lane_id : target.lane_section(section_id)->lanes) {
        created.lanes.emplace_back(lane_id, Lane{});
      }
    }
    return {};
  };
  return command;
}

std::unique_ptr<Command> delete_road(const RoadNetwork& network, RoadId road_id) {
  static constexpr std::string_view kName = "Delete Road";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }

  // §7 closure: connections referencing the road disappear, and where the
  // road was the INCOMING one, their now-dangling connecting roads go too.
  const std::vector<RoadId> doomed = deletion_closure(network, {road_id});

  // Every doomed road is dirty so incremental re-mesh drops (and, on undo,
  // restores) its mesh entry; every junction touching one re-floors.
  DirtySet dirty{.roads = doomed, .topology = true};
  for (const RoadId doomed_id : doomed) {
    for (const JunctionId junction_id : junctions_touching(network, doomed_id)) {
      if (std::ranges::find(dirty.junctions, junction_id) == dirty.junctions.end()) {
        dirty.junctions.push_back(junction_id);
      }
    }
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));
  capture_deletion(network, *command, doomed, std::nullopt);
  return command;
}

std::unique_ptr<Command> split_road(const RoadNetwork& network, RoadId road_id, double split_s) {
  static constexpr std::string_view kName = "Split Road";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail("stale road id");
  }
  if (road->junction.is_valid()) {
    return fail("connecting roads inside a junction cannot be split");
  }
  // Junction-linked ENDS are splittable since the hardening sprint (#92 —
  // the T workflow tees the same main road repeatedly): the head keeps a
  // predecessor-side junction untouched, and a successor-side junction is
  // remapped onto the tail (arm, connections, connecting-road links). Only
  // junctions referencing the road WITHOUT a link on it (foreign data) still
  // refuse — there is no arm to say which end moved.
  std::optional<JunctionId> succ_junction;
  if (road->successor.has_value()) {
    if (const auto* junction_id = std::get_if<JunctionId>(&road->successor->target)) {
      succ_junction = *junction_id;
      const Junction* junction = network.junction(*junction_id);
      if (junction == nullptr) {
        return fail("successor references a stale junction");
      }
      for (const RoadEnd& arm : junction->arms) {
        if (arm.road == road_id && arm.contact == ContactPoint::Start) {
          return fail("road enters the same junction at both ends; recreate the junction first");
        }
      }
    }
  }
  std::optional<JunctionId> pred_junction;
  if (road->predecessor.has_value()) {
    if (const auto* junction_id = std::get_if<JunctionId>(&road->predecessor->target)) {
      pred_junction = *junction_id;
    }
  }
  for (const JunctionId touched : junctions_touching(network, road_id)) {
    if (touched != succ_junction.value_or(JunctionId{}) &&
        touched != pred_junction.value_or(JunctionId{})) {
      return fail("a junction references this road without a link on it; recreate the junction");
    }
  }
  const double length = road->plan_view.length();
  if (split_s <= tol::kLength || split_s >= length - tol::kLength) {
    return fail("split station must lie strictly inside the road");
  }

  // --- geometry: head/tail record lists ---------------------------------
  const auto& records = road->plan_view.records();
  std::size_t covering = 0;
  for (std::size_t i = 0; i < records.size(); ++i) {
    if (records[i].s <= split_s + tol::kLength) {
      covering = i;
    }
  }
  const double local = split_s - records[covering].s;
  const bool cut_inside_record = local > tol::kLength;
  if (cut_inside_record && std::holds_alternative<ParamPoly3Geom>(records[covering].shape)) {
    return fail("cannot split inside a paramPoly3 record in M2");
  }

  ReferenceLine head_line;
  for (std::size_t i = 0; i < covering; ++i) {
    head_line.append(records[i]);
  }
  ReferenceLine tail_line;
  if (cut_inside_record) {
    head_line.append(record_head(records[covering], local));
    tail_line.append(record_tail(records[covering], local, road->plan_view.evaluate(split_s)));
  } else {
    tail_line.append(records[covering]);
  }
  for (std::size_t i = covering + 1; i < records.size(); ++i) {
    tail_line.append(records[i]);
  }

  // --- sections: keep / duplicate-at-boundary / move --------------------
  std::vector<LaneSectionId> kept_sections;
  std::optional<LaneSectionId> spanning; // duplicated onto the new road
  std::vector<LaneSectionId> moved_sections;
  for (const LaneSectionId section_id : road->sections) {
    const double s0 = network.lane_section(section_id)->s0;
    if (s0 < split_s - tol::kLength) {
      kept_sections.push_back(section_id);
      spanning = section_id; // last one starting before the split spans it
    } else {
      moved_sections.push_back(section_id);
    }
  }
  if (!spanning.has_value()) {
    return fail("no lane section covers the split station");
  }
  const double spanning_local = split_s - network.lane_section(*spanning)->s0;

  // --- original road's after value --------------------------------------
  Road original_after = *road;
  original_after.plan_view = head_line;
  original_after.length = head_line.length();
  original_after.sections = kept_sections;
  original_after.elevation = truncate_profile(road->elevation, split_s);
  original_after.superelevation = truncate_profile(road->superelevation, split_s);
  original_after.lane_offset = truncate_profile(road->lane_offset, split_s);
  original_after.authoring_waypoints.reset(); // set below, from the head geometry

  // --- new road blueprint (ids assigned at first apply) ------------------
  Road tail_blueprint;
  tail_blueprint.name = road->name;
  tail_blueprint.plan_view = tail_line;
  tail_blueprint.length = tail_line.length();
  tail_blueprint.elevation = rebase_profile(road->elevation, split_s);
  tail_blueprint.superelevation = rebase_profile(road->superelevation, split_s);
  tail_blueprint.lane_offset = rebase_profile(road->lane_offset, split_s);
  tail_blueprint.successor = road->successor;

  original_after.authoring_waypoints = derive_waypoints(original_after);
  tail_blueprint.authoring_waypoints = derive_waypoints(tail_blueprint);

  // Duplicate of the spanning section for the tail (lane values rebased).
  struct LaneBlueprint {
    Lane value;
  };

  std::vector<LaneBlueprint> duplicated_lanes;
  for (const LaneId lane_id : network.lane_section(*spanning)->lanes) {
    Lane copy = *network.lane(lane_id);
    copy.widths = rebase_profile(copy.widths, spanning_local);
    std::vector<RoadMark> marks;
    for (const RoadMark& mark : copy.road_marks) {
      if (mark.s_offset <= spanning_local + tol::kLength) {
        // Collapses every mark active before the split to offset 0; the last
        // one wins, matching eval semantics.
        if (!marks.empty() && marks.front().s_offset == 0.0) {
          marks.front() = RoadMark{.s_offset = 0.0, .type = mark.type, .width = mark.width};
          continue;
        }
        marks.insert(marks.begin(),
                     RoadMark{.s_offset = 0.0, .type = mark.type, .width = mark.width});
      } else {
        marks.push_back(RoadMark{
            .s_offset = mark.s_offset - spanning_local, .type = mark.type, .width = mark.width});
      }
    }
    copy.road_marks = std::move(marks);
    duplicated_lanes.push_back(LaneBlueprint{.value = std::move(copy)});
  }

  DirtySet split_dirty{.roads = {road_id}, .topology = true};
  if (succ_junction.has_value()) {
    split_dirty.junctions.push_back(*succ_junction);
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(split_dirty));

  // Touched objects: the original road, every moved section, the spanning
  // section's lanes (their successors become identity links into the
  // duplicate), and the far neighbor whose back-link must be re-pointed.
  command->before.roads.emplace_back(road_id, *road);
  for (const LaneSectionId section_id : moved_sections) {
    command->before.sections.emplace_back(section_id, *network.lane_section(section_id));
  }
  for (const LaneId lane_id : network.lane_section(*spanning)->lanes) {
    command->before.lanes.emplace_back(lane_id, *network.lane(lane_id));
  }
  std::optional<RoadId> far_neighbor;
  if (road->successor.has_value()) {
    if (const auto* target = std::get_if<RoadId>(&road->successor->target)) {
      far_neighbor = *target;
      command->before.roads.emplace_back(*target, *network.road(*target));
    }
  }
  std::vector<RoadId> succ_connecting; // connecting roads touching the moved End
  if (succ_junction.has_value()) {
    command->before.junctions.emplace_back(*succ_junction, *network.junction(*succ_junction));
    network.for_each_road([&](RoadId connecting_id, const Road& connecting) {
      if (connecting.junction != *succ_junction) {
        return;
      }
      const auto touches_end = [&](const std::optional<RoadLink>& link) {
        if (!link.has_value() || link->contact != ContactPoint::End) {
          return false;
        }
        const auto* road_target = std::get_if<RoadId>(&link->target);
        return road_target != nullptr && *road_target == road_id;
      };
      if (touches_end(connecting.predecessor) || touches_end(connecting.successor)) {
        succ_connecting.push_back(connecting_id);
        command->before.roads.emplace_back(connecting_id, connecting);
      }
    });
  }

  command->creator = [road_id,
                      original_after = std::move(original_after),
                      tail_blueprint = std::move(tail_blueprint),
                      duplicated_lanes = std::move(duplicated_lanes),
                      moved_sections,
                      split_s,
                      far_neighbor,
                      succ_junction,
                      succ_connecting](RoadNetwork& target, Values& created) -> Expected<void> {
    const std::string odr_id = next_free_road_odr_id(target);
    const RoadId tail_id = target.create_road(tail_blueprint.name, odr_id);
    created.roads.emplace_back(tail_id, Road{});
    {
      Road& tail = *target.road(tail_id);
      const std::string keep_odr = tail.odr_id;
      tail = tail_blueprint;
      tail.odr_id = keep_odr;
      tail.predecessor = RoadLink{.target = road_id, .contact = ContactPoint::End};
    }

    // Boundary duplicate section + lanes (identity odr ids).
    const LaneSectionId dup_section = target.add_lane_section(tail_id, 0.0);
    created.sections.emplace_back(dup_section, LaneSection{});
    for (const LaneBlueprint& blueprint : duplicated_lanes) {
      const LaneId lane_id =
          target.add_lane(dup_section, blueprint.value.odr_id, blueprint.value.type);
      Lane& lane = *target.lane(lane_id);
      const LaneSectionId keep_section = lane.section;
      lane = blueprint.value;
      lane.section = keep_section;
      created.lanes.emplace_back(lane_id, Lane{});
    }

    // Move the tail sections across (same ids — references stay valid).
    Road& tail = *target.road(tail_id);
    for (const LaneSectionId section_id : moved_sections) {
      LaneSection& section = *target.lane_section(section_id);
      section.road = tail_id;
      section.s0 -= split_s;
      tail.sections.push_back(section_id);
    }

    // Rewrite the original road and stitch the seam.
    Road& original = *target.road(road_id);
    const LaneSectionId spanning_id = original_after.sections.back();
    original = original_after;
    original.successor = RoadLink{.target = tail_id, .contact = ContactPoint::Start};
    for (const LaneId lane_id : target.lane_section(spanning_id)->lanes) {
      Lane& lane = *target.lane(lane_id);
      if (lane.odr_id != 0) {
        lane.successor = lane.odr_id; // identity link into the duplicate
      }
    }
    if (far_neighbor.has_value()) {
      Road& neighbor = *target.road(*far_neighbor);
      if (links_to_road(neighbor.predecessor, road_id)) {
        neighbor.predecessor->target = tail_id;
      }
      if (links_to_road(neighbor.successor, road_id)) {
        neighbor.successor->target = tail_id;
      }
    }
    // Successor-side junction: the road's End (its arm end) now lives on the
    // tail — remap the arm, the connection table, and the connecting-road
    // links that referenced it (issue #92). Lane numbering survives intact
    // (the duplicate spanning section keeps identity odr ids), so laneLinks
    // need no rewrite.
    if (succ_junction.has_value()) {
      Junction& junction = *target.junction(*succ_junction);
      for (RoadEnd& arm : junction.arms) {
        if (arm.road == road_id && arm.contact == ContactPoint::End) {
          arm.road = tail_id;
        }
      }
      for (JunctionConnection& connection : junction.connections) {
        if (connection.incoming_road == road_id) {
          connection.incoming_road = tail_id;
        }
      }
      for (const RoadId connecting_id : succ_connecting) {
        Road& connecting = *target.road(connecting_id);
        const auto repoint = [&](std::optional<RoadLink>& link) {
          if (link.has_value() && link->contact == ContactPoint::End) {
            if (auto* road_target = std::get_if<RoadId>(&link->target);
                road_target != nullptr && *road_target == road_id) {
              *road_target = tail_id;
            }
          }
        };
        repoint(connecting.predecessor);
        repoint(connecting.successor);
      }
      // The moved End keeps its junction link — on the tail now (`tail` is
      // the reference bound above; no arena allocation happened since).
      tail.successor = RoadLink{.target = *succ_junction, .contact = ContactPoint::Start};
    }
    return {};
  };
  return command;
}

/// The wider side's total lane width at `station` [m] — what the T-attach
/// gap must clear (docs/design/hardening/t_junction.md).
double half_width_at(const RoadNetwork& network, const Road& road, double station) {
  const LaneSection* covering = nullptr;
  double local = 0.0;
  for (const LaneSectionId section_id : road.sections) {
    const LaneSection* section = network.lane_section(section_id);
    if (section != nullptr && section->s0 <= station + tol::kLength) {
      covering = section;
      local = station - section->s0;
    }
  }
  if (covering == nullptr) {
    return 0.0;
  }
  double left = 0.0;
  double right = 0.0;
  for (const LaneId lane_id : covering->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane == nullptr) {
      continue;
    }
    const double width = eval_profile(lane->widths, local);
    if (lane->odr_id > 0) {
      left += width;
    } else if (lane->odr_id < 0) {
      right += width;
    }
  }
  return std::max(left, right);
}

std::unique_ptr<Command> attach_t_junction(const RoadNetwork& network,
                                           RoadEnd end,
                                           RoadId target_id,
                                           double s,
                                           const TAttachOptions& options) {
  static constexpr std::string_view kName = "Attach T-Junction";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };

  const Road* target = network.road(target_id);
  if (target == nullptr) {
    return fail("stale target road id");
  }
  const Road* attaching = network.road(end.road);
  if (attaching == nullptr) {
    return fail("stale attaching road id");
  }
  if (end.road == target_id) {
    return fail("a road cannot tee into itself");
  }
  const std::array<RoadEnd, 1> attach_end{end};
  if (auto free = ends_link_slots_free(network, attach_end); !free.has_value()) {
    return invalid_command(std::string(kName), free.error());
  }
  if (target->junction.is_valid()) {
    return fail("cannot tee into a junction's connecting road");
  }
  if (attaching->junction.is_valid()) {
    return fail("a junction's connecting road cannot be an arm of another junction");
  }

  double gap = options.gap_m;
  if (gap <= 0.0) {
    // Auto: the junction area must at least span the crossing road's body
    // (design doc §gap auto-sizing).
    const double attach_station = end.contact == ContactPoint::Start ? 0.0 : attaching->length;
    gap = std::max(half_width_at(network, *target, s),
                   half_width_at(network, *attaching, attach_station)) +
          1.0;
  }
  const double length = target->plan_view.length();
  if (s - gap <= tol::kLength || s + gap >= length - tol::kLength) {
    return fail("attach point too close to the target road's end — use an endpoint junction "
                "or attach farther from the ends");
  }

  // Stage state resolved during apply: arena ids are assigned when the
  // splits run, so later builders read them off the mutated network.
  struct Stages {
    RoadId tail;   // [s+gap, …) — the far half, a junction arm
    RoadId middle; // [s−gap, s+gap) — deleted; the junction area
  };

  auto stages = std::make_shared<Stages>();
  const JunctionGenOptions generation = options.generation;

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back(
      [target_id, cut = s + gap](RoadNetwork& net) { return split_road(net, target_id, cut); });
  builders.push_back([target_id, cut = s - gap, stages](RoadNetwork& net) {
    // The first split just pointed the head's successor at the new tail.
    const Road& head = *net.road(target_id);
    stages->tail = std::get<RoadId>(head.successor->target);
    return split_road(net, target_id, cut);
  });
  builders.push_back([target_id, stages](RoadNetwork& net) {
    const Road& head = *net.road(target_id);
    stages->middle = std::get<RoadId>(head.successor->target);
    return delete_road(net, stages->middle);
  });
  builders.push_back([target_id, end, stages, generation](RoadNetwork& net) {
    const std::array<RoadEnd, 3> ends{
        RoadEnd{.road = target_id, .contact = ContactPoint::End},
        RoadEnd{.road = stages->tail, .contact = ContactPoint::Start},
        end,
    };
    return create_junction(net, ends, generation);
  });

  return std::make_unique<CompositeCommand>(
      std::string(kName),
      DirtySet{.roads = {target_id, end.road}, .topology = true},
      std::move(builders));
}

Expected<JunctionPreview> preview_junction(const RoadNetwork& network,
                                           std::span<const RoadEnd> ends,
                                           const JunctionGenOptions& options) {
  if (auto free = ends_link_slots_free(network, ends); !free.has_value()) {
    return tl::unexpected<Error>(free.error());
  }
  auto plan = plan_junction(network, ends, options);
  if (!plan.has_value()) {
    return tl::unexpected<Error>(plan.error());
  }
  return JunctionPreview{.connection_count = static_cast<int>(plan->roads.size()),
                         .dropped_turns = std::move(plan->dropped)};
}

/// Builds the junction record, its connecting roads and the connection table
/// into `target` from `plan`, registering every created object in `created`;
/// also links each incoming end to the junction. Shared by create_junction's
/// creator (regeneration edits existing roads in place instead). Assumes the
/// arm link slots are free — validated at factory time.
Expected<void> materialize_junction(RoadNetwork& target,
                                    Values& created,
                                    std::span<const RoadEnd> ends,
                                    const JunctionPlan& plan) {
  const JunctionId junction_id = target.create_junction(next_free_junction_odr_id(target), "");
  created.junctions.emplace_back(junction_id, Junction{});
  Junction& junction = *target.junction(junction_id);
  junction.arms.assign(ends.begin(), ends.end());

  for (const RoadEnd& end : ends) {
    Road& road = *target.road(end.road);
    const RoadLink link{.target = junction_id, .contact = ContactPoint::Start};
    if (end.contact == ContactPoint::Start) {
      road.predecessor = link;
    } else {
      road.successor = link;
    }
  }

  for (const ConnectingPlan& cp : plan.roads) {
    const RoadId road_id = target.create_road("", next_free_road_odr_id(target));
    created.roads.emplace_back(road_id, Road{});
    Road& road = *target.road(road_id);
    road.plan_view = cp.line;
    road.length = road.plan_view.length();
    road.junction = junction_id;
    // Connecting roads run in driving direction: start touches the incoming
    // arm, end the outgoing arm (12.4.1 laneLink direction rules).
    road.predecessor = RoadLink{.target = cp.from.road, .contact = cp.from.contact};
    road.successor = RoadLink{.target = cp.to.road, .contact = cp.to.contact};

    const LaneSectionId section_id = target.add_lane_section(road_id, 0.0);
    created.sections.emplace_back(section_id, LaneSection{});
    const LaneId center = target.add_lane(section_id, 0, LaneType::None);
    created.lanes.emplace_back(center, Lane{});
    // Single right-hand driving lane carrying the +s (driving-direction) flow.
    const LaneId drive = target.add_lane(section_id, -1, LaneType::Driving);
    created.lanes.emplace_back(drive, Lane{});
    Lane& lane = *target.lane(drive);
    lane.widths.push_back(connecting_lane_width(cp, road.length));
    lane.predecessor = cp.from_lane;
    lane.successor = cp.to_lane;

    junction.connections.push_back(JunctionConnection{.incoming_road = cp.from.road,
                                                      .connecting_road = road_id,
                                                      .contact_point = ContactPoint::Start,
                                                      .lane_links = {{cp.from_lane, -1}}});
  }
  return {};
}

std::unique_ptr<Command> create_junction(const RoadNetwork& network,
                                         std::span<const RoadEnd> ends,
                                         const JunctionGenOptions& options) {
  static constexpr std::string_view kName = "Create Junction";
  if (auto free = ends_link_slots_free(network, ends); !free.has_value()) {
    return invalid_command(std::string(kName), free.error());
  }
  auto plan = plan_junction(network, ends, options);
  if (!plan.has_value()) {
    return invalid_command(std::string(kName), plan.error());
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.topology = true});
  for (const RoadEnd& end : ends) {
    const bool already_touched = std::ranges::any_of(
        command->before.roads, [&](const auto& entry) { return entry.first == end.road; });
    if (!already_touched) {
      command->before.roads.emplace_back(end.road, *network.road(end.road));
    }
  }
  command->creator = [ends = std::vector<RoadEnd>(ends.begin(), ends.end()),
                      plan = std::move(*plan)](RoadNetwork& target,
                                               Values& created) -> Expected<void> {
    return materialize_junction(target, created, ends, plan);
  };
  return command;
}

std::unique_ptr<Command> regenerate_junction(const RoadNetwork& network,
                                             JunctionId junction_id,
                                             const JunctionGenOptions& options) {
  static constexpr std::string_view kName = "Regenerate Junction";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return fail("stale junction id");
  }
  if (junction->arms.empty()) {
    return fail("junction has no recorded arms (loaded from a foreign file); recreate it to edit");
  }
  auto plan = plan_junction(network, junction->arms, options);
  if (!plan.has_value()) {
    return invalid_command(std::string(kName), plan.error());
  }
  // M2 restriction: only geometry/width may change; a different turn set
  // (a lane added or removed on an incoming road) needs a full recreate so
  // ids can be freed. Regeneration edits the existing connecting roads in
  // place, preserving their ids and the connection table.
  if (plan->roads.size() != junction->connections.size()) {
    return fail("regeneration changed the connection count; delete and recreate the junction");
  }

  DirtySet dirty{.junctions = {junction_id}};
  for (const JunctionConnection& connection : junction->connections) {
    dirty.roads.push_back(connection.connecting_road);
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));
  for (std::size_t i = 0; i < plan->roads.size(); ++i) {
    const ConnectingPlan& cp = plan->roads[i];
    const JunctionConnection& connection = junction->connections[i];
    // The plan and the connection table share generation order; a mismatch
    // means the recorded topology drifted from the roads (hand-edited file).
    if (connection.incoming_road != cp.from.road || connection.lane_links.size() != 1 ||
        connection.lane_links.front().first != cp.from_lane) {
      return fail("recorded connections no longer match the arms; recreate the junction");
    }
    const RoadId road_id = connection.connecting_road;
    const Road* road = network.road(road_id);
    if (road == nullptr || road->sections.empty()) {
      return fail("connecting road is missing; recreate the junction");
    }
    Road after = *road;
    after.plan_view = cp.line;
    after.length = after.plan_view.length();
    command->before.roads.emplace_back(road_id, *road);
    command->after.roads.emplace_back(road_id, std::move(after));

    const LaneSection& section = *network.lane_section(road->sections.front());
    for (const LaneId lane_id : section.lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane->odr_id != -1) {
        continue;
      }
      Lane lane_after = *lane;
      lane_after.widths = {connecting_lane_width(cp, after.length)};
      command->before.lanes.emplace_back(lane_id, *lane);
      command->after.lanes.emplace_back(lane_id, std::move(lane_after));
    }
  }
  return command;
}

std::unique_ptr<Command> delete_junction(const RoadNetwork& network, JunctionId junction_id) {
  static constexpr std::string_view kName = "Delete Junction";
  const Junction* junction = network.junction(junction_id);
  if (junction == nullptr) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "stale junction id"});
  }
  // §7 closure: the junction takes its connecting roads (back-reference set)
  // with it; incoming roads survive with their links into it cleared.
  std::vector<RoadId> seeds;
  network.for_each_road([&](RoadId road_id, const Road& road) {
    if (road.junction == junction_id) {
      seeds.push_back(road_id);
    }
  });
  const std::vector<RoadId> doomed = deletion_closure(network, std::move(seeds));

  DirtySet dirty{.roads = doomed, .junctions = {junction_id}, .topology = true};
  for (const RoadId doomed_id : doomed) {
    for (const JunctionId touched : junctions_touching(network, doomed_id)) {
      if (std::ranges::find(dirty.junctions, touched) == dirty.junctions.end()) {
        dirty.junctions.push_back(touched);
      }
    }
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));
  capture_deletion(network, *command, doomed, junction_id);
  return command;
}

std::unique_ptr<Command>
add_lane(const RoadNetwork& network, LaneSectionId section_id, int side, LaneType type) {
  static constexpr std::string_view kName = "Add Lane";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return fail("stale lane-section id");
  }
  if (side != 1 && side != -1) {
    return fail("side must be +1 (left) or -1 (right)");
  }

  int outermost = 0;
  const Lane* outermost_lane = nullptr;
  for (const LaneId lane_id : section->lanes) {
    const Lane& lane = *network.lane(lane_id);
    if (side > 0 ? lane.odr_id > outermost : lane.odr_id < outermost) {
      outermost = lane.odr_id;
      outermost_lane = &lane;
    }
  }
  const int new_odr_id = outermost + side;
  std::vector<Poly3> widths = outermost_lane != nullptr && !outermost_lane->widths.empty()
                                  ? outermost_lane->widths
                                  : std::vector<Poly3>{Poly3{.a = 3.5}};

  auto command = std::make_unique<GenericCommand>(
      std::string(kName), DirtySet{.roads = {section->road}, .topology = true});
  command->before.sections.emplace_back(section_id, *section);
  command->creator = [section_id, new_odr_id, type, widths = std::move(widths)](
                         RoadNetwork& target, Values& created) -> Expected<void> {
    const LaneId lane_id = target.add_lane(section_id, new_odr_id, type);
    if (!lane_id.is_valid()) {
      return make_error(ErrorCode::InvalidArgument, "lane id already occupied");
    }
    target.lane(lane_id)->widths = widths;
    created.lanes.emplace_back(lane_id, Lane{});
    return {};
  };
  return command;
}

std::unique_ptr<Command> remove_lane(const RoadNetwork& network, LaneId lane_id) {
  static constexpr std::string_view kName = "Remove Lane";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  auto context = lane_context(network, lane_id);
  if (!context.has_value()) {
    return invalid_command(std::string(kName), context.error());
  }
  const int odr_id = context->lane.odr_id;
  if (odr_id == 0) {
    return fail("the center lane cannot be removed");
  }
  const LaneSection& section = *network.lane_section(context->section_id);
  for (const LaneId other_id : section.lanes) {
    const int other = network.lane(other_id)->odr_id;
    if ((odr_id > 0 && other > odr_id) || (odr_id < 0 && other < odr_id)) {
      return fail("only the outermost lane of a side can be removed in M2");
    }
  }

  // Junction connections referencing the removed lane by odr id (as the
  // incoming or the connecting side of a lane_link pair) would dangle — drop
  // those pairs; undo restores them exactly (spec 02 §4 integrity).
  std::vector<std::pair<JunctionId, Junction>> junctions_before;
  std::vector<std::pair<JunctionId, Junction>> junctions_after;
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    Junction junction_after = junction;
    bool changed = false;
    for (JunctionConnection& connection : junction_after.connections) {
      changed |= std::erase_if(connection.lane_links, [&](const std::pair<int, int>& link) {
                   return (connection.incoming_road == context->road_id && link.first == odr_id) ||
                          (connection.connecting_road == context->road_id && link.second == odr_id);
                 }) > 0;
    }
    if (changed) {
      junctions_before.emplace_back(junction_id, junction);
      junctions_after.emplace_back(junction_id, std::move(junction_after));
    }
  });

  DirtySet dirty{.roads = {context->road_id}, .topology = true};
  for (const auto& [junction_id, value] : junctions_before) {
    dirty.junctions.push_back(junction_id);
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));
  command->erased.lanes.emplace_back(lane_id, context->lane);
  command->before.junctions = std::move(junctions_before);
  command->after.junctions = std::move(junctions_after);

  LaneSection section_after = section;
  std::erase(section_after.lanes, lane_id);
  command->before.sections.emplace_back(context->section_id, section);
  command->after.sections.emplace_back(context->section_id, std::move(section_after));

  // Clear links in the adjacent sections that point at the removed lane
  // (the writer refuses dangling intra-road links).
  const Road& road = *network.road(context->road_id);
  const auto here = std::ranges::find(road.sections, context->section_id);
  const auto clear_links = [&](LaneSectionId neighbor_id, bool forward) {
    for (const LaneId neighbor_lane_id : network.lane_section(neighbor_id)->lanes) {
      const Lane& lane = *network.lane(neighbor_lane_id);
      const std::optional<int>& link = forward ? lane.successor : lane.predecessor;
      if (link != odr_id) {
        continue;
      }
      Lane after = lane;
      (forward ? after.successor : after.predecessor).reset();
      command->before.lanes.emplace_back(neighbor_lane_id, lane);
      command->after.lanes.emplace_back(neighbor_lane_id, std::move(after));
    }
  };
  if (here != road.sections.begin()) {
    clear_links(*std::prev(here), /*forward=*/true);
  }
  if (here != road.sections.end() && std::next(here) != road.sections.end()) {
    clear_links(*std::next(here), /*forward=*/false);
  }
  return command;
}

std::unique_ptr<Command> set_lane_type(const RoadNetwork& network, LaneId lane_id, LaneType type) {
  static constexpr std::string_view kName = "Set Lane Type";
  auto context = lane_context(network, lane_id);
  if (!context.has_value()) {
    return invalid_command(std::string(kName), context.error());
  }
  Lane after = context->lane;
  after.type = type;
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {context->road_id}});
  command->before.lanes.emplace_back(lane_id, context->lane);
  command->after.lanes.emplace_back(lane_id, std::move(after));
  return command;
}

std::unique_ptr<Command>
set_lane_width(const RoadNetwork& network, LaneId lane_id, double width_m) {
  static constexpr std::string_view kName = "Set Lane Width";
  auto context = lane_context(network, lane_id);
  if (!context.has_value()) {
    return invalid_command(std::string(kName), context.error());
  }
  if (context->lane.odr_id == 0) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "the center lane has no width"});
  }
  if (width_m <= 0.0) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "lane width must be > 0"});
  }
  Lane after = context->lane;
  after.widths = {Poly3{.a = width_m}};
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {context->road_id}});
  command->before.lanes.emplace_back(lane_id, context->lane);
  command->after.lanes.emplace_back(lane_id, std::move(after));
  return command;
}

std::unique_ptr<Command> set_road_mark(const RoadNetwork& network, LaneId lane_id, RoadMark mark) {
  static constexpr std::string_view kName = "Set Road Mark";
  auto context = lane_context(network, lane_id);
  if (!context.has_value()) {
    return invalid_command(std::string(kName), context.error());
  }
  if (mark.width < 0.0 || mark.s_offset < 0.0) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument,
                                 .message = "road mark width and sOffset must be >= 0"});
  }
  // Each lane's mark list describes its OUTER boundary, so
  // asam.net:xodr:1.9.0:road.lane.road_mark.only_outer holds by construction.
  // Additional <roadMark> records stay supported in data: M2 edits the first
  // record only (spec 02 §4), and the edit must keep the list in ascending
  // sOffset order (asam.net:xodr:1.4.0:road.lane.road_mark.elem_asc_order).
  Lane after = context->lane;
  if (after.road_marks.size() > 1 && mark.s_offset >= after.road_marks[1].s_offset) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument,
              .message = "road mark sOffset must stay below the lane's next record"});
  }
  if (after.road_marks.empty()) {
    after.road_marks.push_back(mark);
  } else {
    after.road_marks.front() = mark;
  }
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {context->road_id}});
  command->before.lanes.emplace_back(lane_id, context->lane);
  command->after.lanes.emplace_back(lane_id, std::move(after));
  return command;
}

std::unique_ptr<Command> set_node_elevation(const RoadNetwork& network,
                                            RoadId road_id,
                                            std::size_t waypoint_index,
                                            double z) {
  static constexpr std::string_view kName = "Set Node Elevation";
  const auto fail = [&](Error error) {
    return invalid_command(std::string(kName), std::move(error));
  };
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail(Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  const std::vector<Waypoint> waypoints = effective_waypoints(*road);
  auto stations = waypoint_stations(*road, waypoints.size());
  if (!stations.has_value()) {
    return fail(stations.error());
  }
  if (waypoint_index >= waypoints.size()) {
    return fail(
        Error{.code = ErrorCode::InvalidArgument, .message = "waypoint index out of range"});
  }

  std::vector<double> heights;
  heights.reserve(stations->size());
  for (const double s : *stations) {
    heights.push_back(eval_profile(road->elevation, s));
  }
  heights[waypoint_index] = z;

  Road after = *road;
  after.authoring_waypoints = waypoints;
  // A flat profile is written as no profile at all (the OpenDRIVE default
  // elevation is zero — asam.net:xodr:1.9.0:road.elevation, §10.5.1);
  // otherwise re-fit a C1 cubic through the node (s, z) pairs so the surface
  // reads smoothly (docs/design/m2/02_editing_tools.md §5). Records come out
  // ascending in s (asam.net:xodr:1.4.0:road.elevation.elem_asc_order).
  const bool all_zero = std::ranges::all_of(heights, [](double h) { return std::abs(h) < 1e-12; });
  after.elevation = all_zero ? std::vector<Poly3>{} : fit_elevation_profile(*stations, heights);

  auto command = std::make_unique<GenericCommand>(
      std::string(kName),
      DirtySet{.roads = {road_id}, .junctions = junctions_touching(network, road_id)});
  command->before.roads.emplace_back(road_id, *road);
  command->after.roads.emplace_back(road_id, std::move(after));
  return command;
}

std::unique_ptr<Command> rename_road(const RoadNetwork& network, RoadId road_id, std::string name) {
  static constexpr std::string_view kName = "Rename Road";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  Road after = *road;
  after.name = std::move(name);
  // The name is baked into RoadMesh::name, so a rename IS a mesh change.
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {road_id}});
  command->before.roads.emplace_back(road_id, *road);
  command->after.roads.emplace_back(road_id, std::move(after));
  return command;
}

} // namespace roadmaker::edit
