#include "roadmaker/edit/operations.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
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

std::vector<Waypoint> effective_waypoints(const Road& road) {
  return road.authoring_waypoints.has_value() ? *road.authoring_waypoints : derive_waypoints(road);
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

bool links_to_junction(const std::optional<RoadLink>& link, JunctionId junction) {
  if (!link.has_value()) {
    return false;
  }
  const auto* target = std::get_if<JunctionId>(&link->target);
  return target != nullptr && *target == junction;
}

// ---- waypoint re-fit commands -------------------------------------------------

std::unique_ptr<Command> refit_command(const RoadNetwork& network,
                                       RoadId road_id,
                                       std::string command_name,
                                       std::vector<Waypoint> waypoints) {
  const Road* road = network.road(road_id);
  auto line = fit_clothoid_path(waypoints);
  if (!line.has_value()) {
    return invalid_command(std::move(command_name), line.error());
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

} // namespace

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
  waypoints[index] = to;
  return refit_command(network, road_id, std::string(kName), std::move(waypoints));
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
  waypoints.insert(waypoints.begin() + static_cast<std::ptrdiff_t>(index), at);
  return refit_command(network, road_id, std::string(kName), std::move(waypoints));
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
  waypoints.erase(waypoints.begin() + static_cast<std::ptrdiff_t>(index));
  return refit_command(network, road_id, std::string(kName), std::move(waypoints));
}

std::unique_ptr<Command>
create_road(std::vector<Waypoint> waypoints, LaneProfile profile, std::string name) {
  static constexpr std::string_view kName = "Create Road";
  // Pre-validate the fit so obviously-bad input fails at factory time; the
  // authoring call re-validates against the live network on apply.
  if (auto fit = fit_clothoid_path(waypoints); !fit.has_value()) {
    return invalid_command(std::string(kName), fit.error());
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.topology = true});
  command->creator =
      [waypoints = std::move(waypoints), profile = std::move(profile), name = std::move(name)](
          RoadNetwork& target, Values& created) -> Expected<void> {
    // author_clothoid_road validates everything before its first mutation.
    auto road_id = author_clothoid_road(target, waypoints, profile, name);
    if (!road_id.has_value()) {
      return tl::unexpected<Error>(road_id.error());
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

  auto command = std::make_unique<GenericCommand>(
      std::string(kName),
      DirtySet{.junctions = junctions_touching(network, road_id), .topology = true});

  command->erased.roads.emplace_back(road_id, *road);
  for (const LaneSectionId section_id : road->sections) {
    const LaneSection* section = network.lane_section(section_id);
    command->erased.sections.emplace_back(section_id, *section);
    for (const LaneId lane_id : section->lanes) {
      command->erased.lanes.emplace_back(lane_id, *network.lane(lane_id));
    }
  }

  // Detach every reference into the doomed road: junction connections and
  // other roads' links. Undo restores the exact previous values.
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    const bool references =
        std::ranges::any_of(junction.connections, [road_id](const JunctionConnection& connection) {
          return connection.incoming_road == road_id || connection.connecting_road == road_id;
        });
    if (!references) {
      return;
    }
    Junction after = junction;
    std::erase_if(after.connections, [road_id](const JunctionConnection& connection) {
      return connection.incoming_road == road_id || connection.connecting_road == road_id;
    });
    command->before.junctions.emplace_back(junction_id, junction);
    command->after.junctions.emplace_back(junction_id, std::move(after));
  });
  network.for_each_road([&](RoadId other_id, const Road& other) {
    if (other_id == road_id) {
      return;
    }
    if (!links_to_road(other.predecessor, road_id) && !links_to_road(other.successor, road_id)) {
      return;
    }
    Road after = other;
    if (links_to_road(after.predecessor, road_id)) {
      after.predecessor.reset();
    }
    if (links_to_road(after.successor, road_id)) {
      after.successor.reset();
    }
    command->before.roads.emplace_back(other_id, other);
    command->after.roads.emplace_back(other_id, std::move(after));
  });
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
  if (!junctions_touching(network, road_id).empty()) {
    return fail("roads attached to junctions cannot be split in M2");
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

  auto command = std::make_unique<GenericCommand>(std::string(kName),
                                                  DirtySet{.roads = {road_id}, .topology = true});

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

  command->creator = [road_id,
                      original_after = std::move(original_after),
                      tail_blueprint = std::move(tail_blueprint),
                      duplicated_lanes = std::move(duplicated_lanes),
                      moved_sections,
                      split_s,
                      far_neighbor](RoadNetwork& target, Values& created) -> Expected<void> {
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
    return {};
  };
  return command;
}

std::unique_ptr<Command> create_junction(const RoadNetwork& network,
                                         std::span<const RoadEnd> ends) {
  static constexpr std::string_view kName = "Create Junction";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (ends.size() < 2) {
    return fail("a junction needs at least 2 road ends");
  }
  for (std::size_t i = 0; i < ends.size(); ++i) {
    const Road* road = network.road(ends[i].road);
    if (road == nullptr) {
      return fail("stale road id in road ends");
    }
    for (std::size_t j = i + 1; j < ends.size(); ++j) {
      if (ends[i] == ends[j]) {
        return fail("duplicate road end");
      }
    }
    const auto& slot = ends[i].contact == ContactPoint::Start ? road->predecessor : road->successor;
    if (slot.has_value()) {
      return fail(fmt::format("road '{}' is already linked at that end", road->odr_id));
    }
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.topology = true});
  for (const RoadEnd& end : ends) {
    const bool already_touched = std::ranges::any_of(
        command->before.roads, [&](const auto& entry) { return entry.first == end.road; });
    if (!already_touched) {
      command->before.roads.emplace_back(end.road, *network.road(end.road));
    }
  }
  command->creator = [ends = std::vector<RoadEnd>(ends.begin(), ends.end())](
                         RoadNetwork& target, Values& created) -> Expected<void> {
    const JunctionId junction_id = target.create_junction(next_free_junction_odr_id(target), "");
    created.junctions.emplace_back(junction_id, Junction{});
    for (const RoadEnd& end : ends) {
      Road& road = *target.road(end.road);
      RoadLink link{.target = junction_id, .contact = ContactPoint::Start};
      if (end.contact == ContactPoint::Start) {
        road.predecessor = link;
      } else {
        road.successor = link;
      }
    }
    return {};
  };
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
  auto command = std::make_unique<GenericCommand>(
      std::string(kName), DirtySet{.junctions = {junction_id}, .topology = true});
  command->erased.junctions.emplace_back(junction_id, *junction);
  network.for_each_road([&](RoadId road_id, const Road& road) {
    const bool backref = road.junction == junction_id;
    const bool linked = links_to_junction(road.predecessor, junction_id) ||
                        links_to_junction(road.successor, junction_id);
    if (!backref && !linked) {
      return;
    }
    Road after = road;
    if (backref) {
      after.junction = {};
    }
    if (links_to_junction(after.predecessor, junction_id)) {
      after.predecessor.reset();
    }
    if (links_to_junction(after.successor, junction_id)) {
      after.successor.reset();
    }
    command->before.roads.emplace_back(road_id, road);
    command->after.roads.emplace_back(road_id, std::move(after));
  });
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

  auto command = std::make_unique<GenericCommand>(
      std::string(kName), DirtySet{.roads = {context->road_id}, .topology = true});
  command->erased.lanes.emplace_back(lane_id, context->lane);

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
  Lane after = context->lane;
  after.road_marks = {mark};
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
  const bool all_zero = std::ranges::all_of(heights, [](double h) { return std::abs(h) < 1e-12; });
  if (all_zero) {
    after.elevation.clear();
  } else {
    after.elevation.clear();
    for (std::size_t i = 0; i + 1 < stations->size(); ++i) {
      const double run = (*stations)[i + 1] - (*stations)[i];
      after.elevation.push_back(Poly3{
          .s = (*stations)[i],
          .a = heights[i],
          .b = run > tol::kLength ? (heights[i + 1] - heights[i]) / run : 0.0,
      });
    }
  }

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
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{});
  command->before.roads.emplace_back(road_id, *road);
  command->after.roads.emplace_back(road_id, std::move(after));
  return command;
}

} // namespace roadmaker::edit
