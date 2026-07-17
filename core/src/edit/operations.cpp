#include "roadmaker/edit/operations.hpp"

#include "roadmaker/assets/prop_library.hpp"
#include "roadmaker/edit/assembly.hpp"
#include "roadmaker/edit/connection.hpp"
#include "roadmaker/geometry/profile_fit.hpp"
#include "roadmaker/geometry/road_intersection.hpp"
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
  std::vector<std::pair<ObjectId, Object>> objects;
  std::vector<std::pair<SignalId, Signal>> signals;
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
  for (const auto& [id, value] : values.objects) {
    if (network.object(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale object id");
    }
  }
  for (const auto& [id, value] : values.signals) {
    if (network.signal(id) == nullptr) {
      return make_error(ErrorCode::InvalidArgument, "stale signal id");
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
  for (const auto& [id, value] : values.objects) {
    *network.object(id) = value;
  }
  for (const auto& [id, value] : values.signals) {
    *network.signal(id) = value;
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
  for (const auto& [id, value] : ids.objects) {
    out.objects.emplace_back(id, *network.object(id));
  }
  for (const auto& [id, value] : ids.signals) {
    out.signals.emplace_back(id, *network.signal(id));
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
  for (const auto& [id, value] : values.objects) {
    if (auto restored = network.restore_object(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  for (const auto& [id, value] : values.signals) {
    if (auto restored = network.restore_signal(id, value); !restored.has_value()) {
      return tl::unexpected<Error>(restored.error());
    }
  }
  return {};
}

Expected<void> erase_values_exact(RoadNetwork& network, const Values& values) {
  // Leaf to root, so a partially-visible intermediate state never has a
  // parent without its children. Objects are pure leaves (a road erase would
  // otherwise cascade them) — erase them first.
  for (const auto& [id, value] : values.signals) {
    if (auto erased = network.erase_signal_exact(id); !erased.has_value()) {
      return erased;
    }
  }
  for (const auto& [id, value] : values.objects) {
    if (auto erased = network.erase_object_exact(id); !erased.has_value()) {
      return erased;
    }
  }
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

/// Recycles the reserved slots of everything in `values` — the leaf→root
/// mirror of erase_values_exact, for a command being DISCARDED. Every id here
/// names a slot `erase_values_exact` reserved on the last revert; the guards
/// in release_*_reserved turn a wrong id (already released, or an occupied
/// slot from a not-actually-reverted command) into a harmless no-op, so the
/// void-return, ignore-each-result contract is safe.
void release_values_reserved(RoadNetwork& network, const Values& values) {
  for (const auto& [id, value] : values.signals) {
    (void)network.release_signal_reserved(id);
  }
  for (const auto& [id, value] : values.objects) {
    (void)network.release_object_reserved(id);
  }
  for (const auto& [id, value] : values.lanes) {
    (void)network.release_lane_reserved(id);
  }
  for (const auto& [id, value] : values.sections) {
    (void)network.release_lane_section_reserved(id);
  }
  for (const auto& [id, value] : values.roads) {
    (void)network.release_road_reserved(id);
  }
  for (const auto& [id, value] : values.junctions) {
    (void)network.release_junction_reserved(id);
  }
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

  void discard(RoadNetwork& network) override {
    // Only `created_` reserves slots after a revert: `erased` values were
    // restored (live again) and `before` values overwrote live objects — both
    // sit in occupied slots. `created_` is deliberately NOT cleared: the gen
    // bump makes a double-discard no-op, and the occupied-slot guard makes a
    // misuse-discard on an applied command a no-op, so the precondition is
    // self-enforcing.
    release_values_reserved(network, created_);
  }

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

  void discard(RoadNetwork& network) override {
    // Reverse index order, mirroring revert: a later child's created objects
    // are released before an earlier child's, matching leaf→root teardown.
    for (std::size_t i = children_.size(); i-- > 0;) {
      if (children_[i] != nullptr) {
        children_[i]->discard(network);
      }
    }
  }

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
      // One child that built its own junctions speaks for the composite: the
      // assemblies and attach_t_junction end in create_junction, so the
      // junctions they name are already generated.
      dirty.junctions_are_current =
          dirty.junctions_are_current || child_dirty.junctions_are_current;
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

/// Rebases the tail of a lane's road marks onto a section starting at
/// `split` (RoadMark::s_offset is section-local). Every mark already active
/// at the split collapses to offset 0 and the last one wins, matching
/// evaluation semantics; marks after it keep their spacing.
///
/// Records are copied and only s_offset is rewritten: rebuilding one field
/// by field would drop @color, the <type>/<line> block, and silently any
/// field added to RoadMark later.
std::vector<RoadMark> rebase_marks(std::span<const RoadMark> marks, double split) {
  std::vector<RoadMark> out;
  for (const RoadMark& mark : marks) {
    RoadMark rebased = mark;
    if (mark.s_offset <= split + tol::kLength) {
      rebased.s_offset = 0.0;
      if (!out.empty() && out.front().s_offset == 0.0) {
        out.front() = std::move(rebased);
        continue;
      }
      out.insert(out.begin(), std::move(rebased));
    } else {
      rebased.s_offset = mark.s_offset - split;
      out.push_back(std::move(rebased));
    }
  }
  return out;
}

/// Keeps the marks starting before `split`; the rest belong to the section
/// beginning there (rebase_marks moves them). Mirrors truncate_profile.
std::vector<RoadMark> truncate_marks(std::span<const RoadMark> marks, double split) {
  std::vector<RoadMark> out;
  for (const RoadMark& mark : marks) {
    if (mark.s_offset < split - tol::kLength) {
      out.push_back(mark);
    }
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

/// Captures `doomed` roads onto `command->erased`, with everything they own:
/// sections, lanes, and the objects and signals anchored to them.
///
/// The ownership walk has to be explicit because erase_road_exact does NOT
/// cascade the way erase_road does — it frees exactly the slot it is given.
/// Anything owned but not captured here would survive its road holding a
/// RoadId into an emptied slot. erase_values_exact erases leaf-to-root, so the
/// order things are added in does not matter.
void capture_road_erasure(const RoadNetwork& network,
                          GenericCommand& command,
                          std::span<const RoadId> doomed) {
  for (const RoadId road_id : doomed) {
    const Road* road = network.road(road_id);
    if (road == nullptr) {
      continue;
    }
    command.erased.roads.emplace_back(road_id, *road);
    for (const LaneSectionId section_id : road->sections) {
      const LaneSection* section = network.lane_section(section_id);
      command.erased.sections.emplace_back(section_id, *section);
      for (const LaneId lane_id : section->lanes) {
        command.erased.lanes.emplace_back(lane_id, *network.lane(lane_id));
      }
    }
    for (const ObjectId object_id : objects_of(network, road_id)) {
      command.erased.objects.emplace_back(object_id, *network.object(object_id));
    }
    for (const SignalId signal_id : signals_of(network, road_id)) {
      command.erased.signals.emplace_back(signal_id, *network.signal(signal_id));
    }
  }
}

/// Captures onto `command` everything the closure deletion touches: the
/// doomed roads with everything they own (plus `doomed_junction` when set) as
/// erasures, and — as before/after value edits — surviving junctions whose
/// connections reference a doomed road, and surviving roads whose links (or
/// junction back-reference) point into the deleted set. Undo restores every
/// removed object and link exactly.
void capture_deletion(const RoadNetwork& network,
                      GenericCommand& command,
                      std::span<const RoadId> doomed,
                      std::optional<JunctionId> doomed_junction) {
  capture_road_erasure(network, command, doomed);
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
//
// The contact/fit primitives this generator runs on (contact_state,
// contact_lateral, driving_lanes_at, fit_connector) now live in the connection
// engine (roadmaker/edit/connection.hpp) so the junction, assembly-drop, and
// gap-closing consumers share one authority (gate-extension WS-2). ConnectingPlan
// / JunctionPlan and the elevation/width blends stay here as junction policy.

/// A connecting road the generator will build for one permitted turn.
struct ConnectingPlan {
  RoadEnd from;       // incoming arm
  RoadEnd to;         // outgoing arm
  int from_lane = 0;  // incoming lane odr id on `from`
  int to_lane = 0;    // outgoing lane odr id on `to`
  ReferenceLine line; // driving-direction reference line, from → to
  double start_width = 0.0;
  double end_width = 0.0;
  // Elevation boundary data, measured along the connecting road's own +s
  // (driving direction): z and dz/ds of the linked arm at its cut face. The
  // generated cubic matches both so the surface enters/exits the junction
  // smoothly (asam.net:xodr:1.8.0:junctions.elevation_grid.entry_exit_smoothness).
  double start_z = 0.0;
  double start_grade = 0.0;
  double end_z = 0.0;
  double end_grade = 0.0;
};

struct JunctionPlan {
  std::vector<ConnectingPlan> roads;
  std::vector<std::string> dropped;
};

double end_distance(const ContactState& a, const ContactState& b) {
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
  std::vector<ContactState> arm_ends;
  arm_ends.reserve(ends.size());
  for (const RoadEnd& end : ends) {
    auto geom = contact_state(network, end);
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
      std::vector<ContactLane> incoming =
          driving_lanes_at(network, ends[i], arm_ends[i], /*incoming=*/true);
      std::vector<ContactLane> outgoing =
          driving_lanes_at(network, ends[j], arm_ends[j], /*incoming=*/false);
      const std::size_t pairs = std::min(incoming.size(), outgoing.size());
      // Lane discipline when the movement cannot use every lane: right
      // turns depart from / arrive at the OUTERMOST (curb) lanes — the
      // lists' natural curb-in order — but LEFT turns use the INNERMOST
      // lanes, or their path would cross the inner lanes' through
      // connections (a lane-order swap smooth_fit forbids; see the quality
      // matrix's fan-out crossing invariant).
      const double deflection =
          std::remainder(arm_ends[j].out_hdg - arm_ends[i].into_hdg, 2.0 * std::numbers::pi);
      if (deflection > 10.0 * std::numbers::pi / 180.0) { // left turn
        if (incoming.size() > pairs) {
          incoming.erase(incoming.begin(),
                         incoming.begin() + static_cast<std::ptrdiff_t>(incoming.size() - pairs));
        }
        if (outgoing.size() > pairs) {
          outgoing.erase(outgoing.begin(),
                         outgoing.begin() + static_cast<std::ptrdiff_t>(outgoing.size() - pairs));
        }
      }
      const Road& road_in = *network.road(ends[i].road);
      const Road& road_out = *network.road(ends[j].road);
      // Elevation and grade at the cut faces, signed along the connecting
      // road's driving direction (a Start-contact arm runs opposite to it).
      // contact_state precomputes z and the +s grade, so these read off it.
      const double z_in = arm_ends[i].z;
      const double z_out = arm_ends[j].z;
      const double g_in = (ends[i].contact == ContactPoint::End ? 1.0 : -1.0) * arm_ends[i].grade;
      const double g_out =
          (ends[j].contact == ContactPoint::Start ? 1.0 : -1.0) * arm_ends[j].grade;
      for (std::size_t k = 0; k < pairs; ++k) {
        // Anchor the reference line on the linked lanes' inner boundaries:
        // the connecting road carries one right-hand lane (id -1) spanning
        // [-width, 0], so a reference line laid on the inner boundary makes
        // the connecting lane occupy exactly the linked lane's cross section
        // at both contacts (smooth_fit, see ContactLane).
        const std::array<double, 2> a = contact_lateral(arm_ends[i], incoming[k].inner_t);
        const std::array<double, 2> b = contact_lateral(arm_ends[j], outgoing[k].inner_t);
        const auto describe = [&] {
          return fmt::format("{}→{} (lane {}→{})",
                             road_in.odr_id,
                             road_out.odr_id,
                             incoming[k].odr_id,
                             outgoing[k].odr_id);
        };
        // The junction G1 connector fit — position + heading — now via the
        // shared engine primitive (byte-identical to the inline sequence).
        auto connector =
            fit_connector(ConnectorEndpoint{.x = a[0], .y = a[1], .heading = arm_ends[i].into_hdg},
                          ConnectorEndpoint{.x = b[0], .y = b[1], .heading = arm_ends[j].out_hdg},
                          ConnectorParams{.max_loop_factor = options.max_loop_factor});
        if (!connector.has_value()) {
          plan.dropped.push_back(describe() + ": " + connector.error().message);
          continue;
        }
        plan.roads.push_back(ConnectingPlan{.from = ends[i],
                                            .to = ends[j],
                                            .from_lane = incoming[k].odr_id,
                                            .to_lane = outgoing[k].odr_id,
                                            .line = std::move(connector->line),
                                            .start_width = incoming[k].width,
                                            .end_width = outgoing[k].width,
                                            .start_z = z_in,
                                            .start_grade = g_in,
                                            .end_z = z_out,
                                            .end_grade = g_out});
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

/// A connecting road's elevation profile: the cubic Hermite matching the
/// arms' z AND grade at both cut faces (entry_exit_smoothness, see
/// ConnectingPlan). A flat result (everything ~0) yields no profile — the
/// OpenDRIVE default, keeping flat networks byte-identical.
std::vector<Poly3> connecting_elevation(const ConnectingPlan& plan, double length) {
  const bool flat = std::abs(plan.start_z) < tol::kLength && std::abs(plan.end_z) < tol::kLength &&
                    std::abs(plan.start_grade) < tol::kLength &&
                    std::abs(plan.end_grade) < tol::kLength;
  if (flat || length <= tol::kLength) {
    return {};
  }
  const double z0 = plan.start_z, z1 = plan.end_z;
  const double g0 = plan.start_grade, g1 = plan.end_grade;
  return {Poly3{.s = 0.0,
                .a = z0,
                .b = g0,
                .c = ((3.0 * (z1 - z0)) - (((2.0 * g0) + g1) * length)) / (length * length),
                .d = ((2.0 * (z0 - z1)) + ((g0 + g1) * length)) / (length * length * length)}};
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

std::unique_ptr<Command> insert_node_at(const RoadNetwork& network, RoadId road_id, double s) {
  static constexpr std::string_view kName = "Insert Node";
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  const double length = road->plan_view.length();
  if (s <= 0.0 || s >= length) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "insert station is outside the road"});
  }

  std::vector<Waypoint> waypoints = effective_waypoints(*road);
  const auto stations = waypoint_stations(*road, waypoints.size());
  if (!stations.has_value()) {
    return invalid_command(std::string(kName), stations.error());
  }
  for (const double station : *stations) {
    if (std::abs(s - station) < kMinNodeSpacingM) {
      return invalid_command(
          std::string(kName),
          Error{.code = ErrorCode::InvalidArgument,
                .message = fmt::format("a node already exists within {:g} m of this point",
                                       kMinNodeSpacingM)});
    }
  }

  // Insert before the first node past s.
  std::size_t index = 0;
  while (index < stations->size() && (*stations)[index] < s) {
    ++index;
  }

  // Pin the heading at EVERY node from the current curve — this is what makes
  // the re-fit reproduce untouched records exactly and split only the covering
  // record. The new node's pose comes from evaluating the curve at s.
  std::vector<double> headings;
  headings.reserve(waypoints.size() + 1);
  for (const double station : *stations) {
    headings.push_back(road->plan_view.evaluate(station).hdg);
  }
  const PathPoint point = road->plan_view.evaluate(s);
  headings.insert(headings.begin() + static_cast<std::ptrdiff_t>(index), point.hdg);
  waypoints.insert(waypoints.begin() + static_cast<std::ptrdiff_t>(index),
                   Waypoint{.x = point.x, .y = point.y});
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
  // junctions_are_current: capture_deletion strips the doomed connections AND
  // arms itself, so a survivor's arm list is already correct — regenerating it
  // here would replan a junction the user just tore an arm off of.
  DirtySet dirty{.roads = doomed, .topology = true, .junctions_are_current = true};
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

std::unique_ptr<Command> translate_roads(const RoadNetwork& network,
                                         std::span<const RoadId> road_ids,
                                         double dx,
                                         double dy) {
  static constexpr std::string_view kName = "Move Road";
  if (road_ids.empty()) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "no roads to move"});
  }

  // Distinct, live ids (a duplicate would otherwise be shifted twice).
  std::vector<RoadId> moved;
  moved.reserve(road_ids.size());
  for (const RoadId id : road_ids) {
    if (network.road(id) == nullptr) {
      return invalid_command(std::string(kName),
                             Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
    }
    if (std::ranges::find(moved, id) == moved.end()) {
      moved.push_back(id);
    }
  }

  // Junction roads can't be moved as free bodies — their geometry is generated
  // from the arm poses, and an approach road's pose is welded to the junction.
  // Refuse, naming the first offending junction (junctions_touching covers the
  // connecting-road case, the arm/approach case, and the junction back-ref).
  for (const RoadId id : moved) {
    const std::vector<JunctionId> touched = junctions_touching(network, id);
    if (!touched.empty()) {
      const Junction* junction = network.junction(touched.front());
      return invalid_command(
          std::string(kName),
          Error{.code = ErrorCode::InvalidArgument,
                .message =
                    fmt::format("road {} participates in junction {} — junction roads can't be "
                                "moved; delete the junction or move its free end nodes instead",
                                network.road(id)->odr_id,
                                junction != nullptr ? junction->odr_id : std::string("?"))});
    }
  }

  const auto in_set = [&moved](RoadId id) { return std::ranges::find(moved, id) != moved.end(); };
  // A road-level link is broken when it leaves the moved set — the two ends no
  // longer meet. Links between two roads moving together survive (both shift by
  // the same delta). Lane-level links are left as delete_road leaves them.
  const auto links_out = [&](const std::optional<RoadLink>& link) {
    if (!link.has_value()) {
      return false;
    }
    const auto* target = std::get_if<RoadId>(&link->target);
    return target != nullptr && !in_set(*target);
  };
  const auto links_in = [&](const std::optional<RoadLink>& link) {
    if (!link.has_value()) {
      return false;
    }
    const auto* target = std::get_if<RoadId>(&link->target);
    return target != nullptr && in_set(*target);
  };

  // Unmoved roads whose links point INTO the set: their back-links break too,
  // cleared in the SAME command so break+move is one undo step.
  std::vector<std::pair<RoadId, Road>> far_before;
  std::vector<std::pair<RoadId, Road>> far_after;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (in_set(id) || (!links_in(road.predecessor) && !links_in(road.successor))) {
      return;
    }
    Road after = road;
    if (links_in(after.predecessor)) {
      after.predecessor.reset();
    }
    if (links_in(after.successor)) {
      after.successor.reset();
    }
    far_before.emplace_back(id, road);
    far_after.emplace_back(id, std::move(after));
  });

  DirtySet dirty;
  dirty.roads = moved;
  for (const auto& [id, road] : far_before) {
    dirty.roads.push_back(id);
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));
  for (const RoadId id : moved) {
    const Road& original = *network.road(id);
    Road after = original;

    // Shift every geometry record's start position; headings, lengths and s are
    // untouched (append recomputes s from the unchanged running length, so the
    // reference line reproduces byte-for-byte on undo from the before snapshot).
    ReferenceLine shifted;
    for (GeometryRecord record : original.plan_view.records()) {
      record.x += dx;
      record.y += dy;
      shifted.append(record);
    }
    after.plan_view = std::move(shifted);

    if (after.authoring_waypoints.has_value()) {
      for (Waypoint& waypoint : *after.authoring_waypoints) {
        waypoint.x += dx;
        waypoint.y += dy;
      }
    }

    if (links_out(after.predecessor)) {
      after.predecessor.reset();
    }
    if (links_out(after.successor)) {
      after.successor.reset();
    }

    command->before.roads.emplace_back(id, original);
    command->after.roads.emplace_back(id, std::move(after));
  }
  for (std::size_t i = 0; i < far_before.size(); ++i) {
    command->before.roads.push_back(std::move(far_before[i]));
    command->after.roads.push_back(std::move(far_after[i]));
  }
  return command;
}

std::unique_ptr<Command>
translate_road(const RoadNetwork& network, RoadId road, double dx, double dy) {
  const std::array<RoadId, 1> ids{road};
  return translate_roads(network, ids, dx, dy);
}

std::unique_ptr<Command> rotate_road(
    const RoadNetwork& network, RoadId road_id, double angle, double pivot_x, double pivot_y) {
  static constexpr std::string_view kName = "Rotate Road";
  const Road* original_ptr = network.road(road_id);
  if (original_ptr == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }

  // Junction roads have generated poses (see translate_roads) — refuse.
  const std::vector<JunctionId> touched = junctions_touching(network, road_id);
  if (!touched.empty()) {
    const Junction* junction = network.junction(touched.front());
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument,
              .message =
                  fmt::format("road {} participates in junction {} — junction roads can't be "
                              "rotated; delete the junction or move its free end nodes instead",
                              original_ptr->odr_id,
                              junction != nullptr ? junction->odr_id : std::string("?"))});
  }

  const double cos_a = std::cos(angle);
  const double sin_a = std::sin(angle);
  const auto rot_x = [&](double x, double y) {
    return pivot_x + (cos_a * (x - pivot_x)) - (sin_a * (y - pivot_y));
  };
  const auto rot_y = [&](double x, double y) {
    return pivot_y + (sin_a * (x - pivot_x)) + (cos_a * (y - pivot_y));
  };

  // A single road rotating alone breaks every road-level link it has (the ends
  // no longer meet). links_to(road_id) finds neighbours pointing INTO it so
  // their back-links clear in the same command (break + rotate = one undo step).
  const auto links_to_road = [&](const std::optional<RoadLink>& link) {
    if (!link.has_value()) {
      return false;
    }
    const auto* target = std::get_if<RoadId>(&link->target);
    return target != nullptr && *target == road_id;
  };
  const auto is_road_link = [](const std::optional<RoadLink>& link) {
    return link.has_value() && std::get_if<RoadId>(&link->target) != nullptr;
  };

  std::vector<std::pair<RoadId, Road>> far_before;
  std::vector<std::pair<RoadId, Road>> far_after;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (id == road_id || (!links_to_road(road.predecessor) && !links_to_road(road.successor))) {
      return;
    }
    Road after = road;
    if (links_to_road(after.predecessor)) {
      after.predecessor.reset();
    }
    if (links_to_road(after.successor)) {
      after.successor.reset();
    }
    far_before.emplace_back(id, road);
    far_after.emplace_back(id, std::move(after));
  });

  DirtySet dirty;
  dirty.roads.push_back(road_id);
  for (const auto& [id, road] : far_before) {
    dirty.roads.push_back(id);
  }

  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));

  const Road& original = *original_ptr;
  Road after = original;
  ReferenceLine rotated;
  for (GeometryRecord record : original.plan_view.records()) {
    const double nx = rot_x(record.x, record.y);
    const double ny = rot_y(record.x, record.y);
    record.x = nx;
    record.y = ny;
    // hdg' = hdg + angle, normalized to [-pi, pi] (atan2 is periodic, so the
    // shape variant — arc/spiral/paramPoly3, all in the local heading frame —
    // needs no change).
    record.hdg = std::atan2(std::sin(record.hdg + angle), std::cos(record.hdg + angle));
    rotated.append(record);
  }
  after.plan_view = std::move(rotated);

  if (after.authoring_waypoints.has_value()) {
    for (Waypoint& waypoint : *after.authoring_waypoints) {
      const double nx = rot_x(waypoint.x, waypoint.y);
      const double ny = rot_y(waypoint.x, waypoint.y);
      waypoint.x = nx;
      waypoint.y = ny;
    }
  }

  if (is_road_link(after.predecessor)) {
    after.predecessor.reset();
  }
  if (is_road_link(after.successor)) {
    after.successor.reset();
  }

  command->before.roads.emplace_back(road_id, original);
  command->after.roads.emplace_back(road_id, std::move(after));
  for (std::size_t i = 0; i < far_before.size(); ++i) {
    command->before.roads.push_back(std::move(far_before[i]));
    command->after.roads.push_back(std::move(far_after[i]));
  }
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
    copy.road_marks = rebase_marks(copy.road_marks, spanning_local);
    duplicated_lanes.push_back(LaneBlueprint{.value = std::move(copy)});
  }

  // junctions_are_current: the split remaps the junction's arms and
  // incoming_road onto the tail itself (below), so the connection table is
  // already correct — a regeneration would only re-fit geometry that did not
  // move.
  DirtySet split_dirty{.roads = {road_id}, .topology = true, .junctions_are_current = true};
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

// ---- merge (docs/design/m3a/06_topology_editing.md) ---------------------------

namespace {

/// Inverse of rebase_profile: shifts a profile from local coordinates (starting
/// at 0) to a global origin at `shift` [m]. Coefficients are unchanged.
std::vector<Poly3> shift_profile(std::span<const Poly3> profile, double shift) {
  std::vector<Poly3> out;
  out.reserve(profile.size());
  for (const Poly3& poly : profile) {
    out.push_back(Poly3{.s = poly.s + shift, .a = poly.a, .b = poly.b, .c = poly.c, .d = poly.d});
  }
  return out;
}

/// Value of a global-s profile at `s` (0 when empty).
double profile_value_at(std::span<const Poly3> profile, double s) {
  const Poly3* covering = nullptr;
  for (const Poly3& poly : profile) {
    if (poly.s <= s + tol::kLength) {
      covering = &poly;
    }
  }
  return covering != nullptr ? covering->eval(s) : 0.0;
}

double profile_grade_at(std::span<const Poly3> profile, double s) {
  const Poly3* covering = nullptr;
  for (const Poly3& poly : profile) {
    if (poly.s <= s + tol::kLength) {
      covering = &poly;
    }
  }
  return covering != nullptr ? covering->eval_derivative(s) : 0.0;
}

/// The road mark active at section-local offset `offset` (the last one starting
/// at or before it), or a default mark.
RoadMark active_mark(std::span<const RoadMark> marks, double offset) {
  RoadMark active;
  for (const RoadMark& mark : marks) {
    if (mark.s_offset <= offset + tol::kLength) {
      active = mark;
    }
  }
  active.s_offset = 0.0; // compare shape, not position
  return active;
}

tl::unexpected<Error> merge_error(std::string message) {
  return tl::unexpected<Error>(
      Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
}

} // namespace

Expected<void> check_mergeable(const RoadNetwork& network, RoadId a_id, RoadId b_id) {
  if (a_id == b_id) {
    return merge_error("cannot merge a road with itself");
  }
  const Road* a = network.road(a_id);
  const Road* b = network.road(b_id);
  if (a == nullptr || b == nullptr) {
    return merge_error("stale road id");
  }
  if (a->plan_view.empty() || b->plan_view.empty()) {
    return merge_error("a road has no geometry");
  }

  // v1: any junction involvement is refused (junction-aware merge is a
  // follow-up). junctions_touching covers connecting roads, arms, and links.
  if (!junctions_touching(network, a_id).empty() || !junctions_touching(network, b_id).empty()) {
    return merge_error(
        fmt::format("road {} or {} participates in a junction — merging junction roads isn't "
                    "supported yet; delete the junction first",
                    a->odr_id,
                    b->odr_id));
  }

  // The joining ends (a's End, b's Start) must be free of third-party links.
  if (a->successor.has_value()) {
    const auto* target = std::get_if<RoadId>(&a->successor->target);
    if (target == nullptr || *target != b_id) {
      return merge_error(
          fmt::format("road {}'s end is already connected to another road", a->odr_id));
    }
  }
  if (b->predecessor.has_value()) {
    const auto* target = std::get_if<RoadId>(&b->predecessor->target);
    if (target == nullptr || *target != a_id) {
      return merge_error(
          fmt::format("road {}'s start is already connected to another road", b->odr_id));
    }
  }

  const PathPoint a_end = a->plan_view.evaluate(a->plan_view.length());
  const PathPoint a_start = a->plan_view.evaluate(0.0);
  const PathPoint b_start = b->plan_view.evaluate(0.0);
  const PathPoint b_end = b->plan_view.evaluate(b->plan_view.length());

  const double gap = std::hypot(a_end.x - b_start.x, a_end.y - b_start.y);
  if (gap > tol::kMergePositionGap) {
    const double gap_end_end = std::hypot(a_end.x - b_end.x, a_end.y - b_end.y);
    const double gap_start_start = std::hypot(a_start.x - b_start.x, a_start.y - b_start.y);
    if (std::min(gap_end_end, gap_start_start) <= gap) {
      return merge_error("the roads meet end-to-end — reverse one first (coming soon)");
    }
    return merge_error(
        fmt::format("the joining ends are {:.2f} m apart — move them together first", gap));
  }

  const double heading_gap =
      std::abs(std::remainder(a_end.hdg - b_start.hdg, 2.0 * std::numbers::pi));
  if (heading_gap > tol::kMergeHeading) {
    return merge_error("the joining ends' headings differ — align them first");
  }

  // Seam profile equality, lane by lane (v1: values must match; sections are
  // concatenated, never coalesced).
  const LaneSection* a_seam = network.lane_section(a->sections.back());
  const LaneSection* b_seam = network.lane_section(b->sections.front());
  const double a_seam_local = a->plan_view.length() - a_seam->s0;

  if (a_seam->lanes.size() != b_seam->lanes.size()) {
    return merge_error("the roads have different lane counts at the seam");
  }
  constexpr double kSeamValue = 1e-3;
  for (const LaneId a_lane_id : a_seam->lanes) {
    const Lane& a_lane = *network.lane(a_lane_id);
    const Lane* b_lane = nullptr;
    for (const LaneId b_lane_id : b_seam->lanes) {
      if (network.lane(b_lane_id)->odr_id == a_lane.odr_id) {
        b_lane = network.lane(b_lane_id);
        break;
      }
    }
    if (b_lane == nullptr) {
      return merge_error(
          fmt::format("lane {} is missing on the other road at the seam", a_lane.odr_id));
    }
    if (a_lane.type != b_lane->type) {
      return merge_error(fmt::format("lane {} changes type at the seam", a_lane.odr_id));
    }
    if (std::abs(profile_value_at(a_lane.widths, a_seam_local) -
                 profile_value_at(b_lane->widths, 0.0)) > kSeamValue) {
      return merge_error(fmt::format("lane {} width doesn't match at the seam", a_lane.odr_id));
    }
    if (active_mark(a_lane.road_marks, a_seam_local) != active_mark(b_lane->road_marks, 0.0)) {
      return merge_error(fmt::format("lane {} road mark changes at the seam", a_lane.odr_id));
    }
  }

  if (std::abs(profile_value_at(a->lane_offset, a->plan_view.length()) -
               profile_value_at(b->lane_offset, 0.0)) > kSeamValue) {
    return merge_error("the lane offset doesn't match at the seam");
  }
  if (std::abs(profile_value_at(a->elevation, a->plan_view.length()) -
               profile_value_at(b->elevation, 0.0)) > kSeamValue ||
      std::abs(profile_grade_at(a->elevation, a->plan_view.length()) -
               profile_grade_at(b->elevation, 0.0)) > kSeamValue) {
    return merge_error("the elevation doesn't match at the seam");
  }
  return {};
}

std::unique_ptr<Command> merge_roads(const RoadNetwork& network, RoadId a_id, RoadId b_id) {
  static constexpr std::string_view kName = "Merge Roads";
  if (auto ok = check_mergeable(network, a_id, b_id); !ok.has_value()) {
    return invalid_command(std::string(kName), ok.error());
  }
  const Road* a = network.road(a_id);
  const Road* b = network.road(b_id);
  const double a_length = a->plan_view.length();

  // Re-anchor b's plan view onto a's end pose (weld absorbs the residual).
  const PathPoint a_end = a->plan_view.evaluate(a_length);
  const PathPoint b_start = b->plan_view.evaluate(0.0);
  const double dtheta = std::remainder(a_end.hdg - b_start.hdg, 2.0 * std::numbers::pi);
  const double cos_t = std::cos(dtheta);
  const double sin_t = std::sin(dtheta);
  ReferenceLine merged_line;
  for (const GeometryRecord& record : a->plan_view.records()) {
    merged_line.append(record);
  }
  for (const GeometryRecord& record : b->plan_view.records()) {
    const double dx = record.x - b_start.x;
    const double dy = record.y - b_start.y;
    GeometryRecord welded = record;
    welded.x = a_end.x + ((cos_t * dx) - (sin_t * dy));
    welded.y = a_end.y + ((sin_t * dx) + (cos_t * dy));
    welded.hdg = record.hdg + dtheta;
    merged_line.append(welded);
  }

  // Far neighbor at b's far end whose back-link must re-point onto a.
  std::optional<RoadId> far_neighbor;
  if (b->successor.has_value()) {
    if (const auto* target = std::get_if<RoadId>(&b->successor->target)) {
      far_neighbor = *target;
    }
  }

  Road merged_after = *a;
  merged_after.plan_view = merged_line;
  merged_after.length = merged_line.length();
  merged_after.elevation = a->elevation;
  for (const Poly3& poly : shift_profile(b->elevation, a_length)) {
    merged_after.elevation.push_back(poly);
  }
  merged_after.superelevation = a->superelevation;
  for (const Poly3& poly : shift_profile(b->superelevation, a_length)) {
    merged_after.superelevation.push_back(poly);
  }
  merged_after.lane_offset = a->lane_offset;
  for (const Poly3& poly : shift_profile(b->lane_offset, a_length)) {
    merged_after.lane_offset.push_back(poly);
  }
  merged_after.successor = b->successor; // a inherits b's far-end link
  merged_after.authoring_waypoints = derive_waypoints(merged_after);

  // DirtySet + captured objects.
  DirtySet dirty{.roads = {a_id, b_id}, .topology = true};
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));

  // Undo snapshots: a and its seam lanes (successors change), the far neighbor.
  command->before.roads.emplace_back(a_id, *a);
  const LaneSectionId a_seam_section = a->sections.back();
  for (const LaneId lane_id : network.lane_section(a_seam_section)->lanes) {
    command->before.lanes.emplace_back(lane_id, *network.lane(lane_id));
  }
  if (far_neighbor.has_value() && *far_neighbor != a_id && *far_neighbor != b_id) {
    command->before.roads.emplace_back(*far_neighbor, *network.road(*far_neighbor));
  }

  // Erasures: b, all its sections, all its lanes.
  command->erased.roads.emplace_back(b_id, *b);
  for (const LaneSectionId section_id : b->sections) {
    const LaneSection* section = network.lane_section(section_id);
    command->erased.sections.emplace_back(section_id, *section);
    for (const LaneId lane_id : section->lanes) {
      command->erased.lanes.emplace_back(lane_id, *network.lane(lane_id));
    }
  }

  // Snapshot the data the creator needs (b dies before the creator's copies).
  struct SectionBlueprint {
    double s0 = 0.0;
    bool first = false;
    std::vector<Lane> lanes;
  };

  std::vector<SectionBlueprint> section_blueprints;
  for (std::size_t i = 0; i < b->sections.size(); ++i) {
    const LaneSection* section = network.lane_section(b->sections[i]);
    SectionBlueprint blueprint{.s0 = section->s0 + a_length, .first = (i == 0)};
    for (const LaneId lane_id : section->lanes) {
      blueprint.lanes.push_back(*network.lane(lane_id));
    }
    section_blueprints.push_back(std::move(blueprint));
  }

  command->creator = [a_id,
                      a_seam_section,
                      far_neighbor,
                      b_id,
                      merged_after = std::move(merged_after),
                      section_blueprints = std::move(section_blueprints)](
                         RoadNetwork& target, Values& created) -> Expected<void> {
    // Copy b's sections and lanes onto a (new ids; odr ids preserved).
    std::vector<LaneSectionId> new_sections;
    for (const SectionBlueprint& blueprint : section_blueprints) {
      const LaneSectionId section_id = target.add_lane_section(a_id, blueprint.s0);
      created.sections.emplace_back(section_id, LaneSection{});
      new_sections.push_back(section_id);
      for (const Lane& lane_value : blueprint.lanes) {
        const LaneId lane_id = target.add_lane(section_id, lane_value.odr_id, lane_value.type);
        Lane& lane = *target.lane(lane_id);
        const LaneSectionId keep_section = lane.section;
        lane = lane_value;
        lane.section = keep_section;
        // The first copied section is the seam continuation: identity link back
        // into a's original last section (mirror of split's stitching).
        if (blueprint.first && lane.odr_id != 0) {
          lane.predecessor = lane.odr_id;
        }
        created.lanes.emplace_back(lane_id, Lane{});
      }
    }

    // Rewrite a: geometry/profiles/links, seam identity successors, appended
    // section list.
    Road& merged = *target.road(a_id);
    merged = merged_after;
    for (const LaneSectionId section_id : new_sections) {
      merged.sections.push_back(section_id);
    }
    for (const LaneId lane_id : target.lane_section(a_seam_section)->lanes) {
      Lane& lane = *target.lane(lane_id);
      if (lane.odr_id != 0) {
        lane.successor = lane.odr_id; // identity link into the first copied section
      }
    }

    // Far neighbor's back-link re-points from b onto a.
    if (far_neighbor.has_value() && far_neighbor != a_id) {
      if (Road* neighbor = target.road(*far_neighbor)) {
        if (links_to_road(neighbor->predecessor, b_id)) {
          neighbor->predecessor->target = a_id;
        }
        if (links_to_road(neighbor->successor, b_id)) {
          neighbor->successor->target = a_id;
        }
      }
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

double t_attach_gap(const RoadNetwork& network,
                    RoadEnd end,
                    RoadId target_id,
                    double s,
                    const TAttachOptions& options) {
  if (options.gap_m > 0.0) {
    return options.gap_m;
  }
  const Road* target = network.road(target_id);
  const Road* attaching = network.road(end.road);
  if (target == nullptr || attaching == nullptr) {
    return 0.0;
  }
  constexpr double kPi = std::numbers::pi;
  const PathPoint corner = target->plan_view.evaluate(s);
  const PathPoint branch_face = attaching->plan_view.evaluate(
      end.contact == ContactPoint::Start ? 0.0 : attaching->plan_view.length());

  // Auto (design doc §gap auto-sizing): the junction area must span the
  // crossing road's body (width bound) AND give both generated turn
  // directions room to stay drivable (turning bound). A circular turn of
  // radius r deflecting by Δθ needs tangent legs of r·tan(Δθ/2) on each
  // side of the corner; the leg along the target is the gap. Δθ is clamped
  // to 150° — beyond that the tangent length diverges while the fitted
  // clothoid resolves the remainder as an S-curve near the branch mouth.
  const double attach_station = end.contact == ContactPoint::Start ? 0.0 : attaching->length;
  const double width_bound = std::max(half_width_at(network, *target, s),
                                      half_width_at(network, *attaching, attach_station)) +
                             1.0;
  const double branch_in_hdg =
      end.contact == ContactPoint::Start ? branch_face.hdg + kPi : branch_face.hdg;
  const double psi = std::abs(std::remainder(branch_in_hdg - corner.hdg, 2.0 * kPi));
  constexpr double kMaxDeflection = 150.0 * kPi / 180.0;
  // On a curved target the cut faces rotate with the reference line, so a
  // turn's real deflection grows by the heading swept across the gap
  // (gap·|κ|); attaching on the concave (inside) side is the cramped case —
  // the arms rotate TOWARD the branch, so the sweep counts double there.
  // gap appears on both sides of the equation — 3 fixed-point rounds
  // converge far below tol for every drivable target curvature.
  const double lateral_sign = (std::cos(corner.hdg) * (branch_face.y - corner.y)) -
                              (std::sin(corner.hdg) * (branch_face.x - corner.x));
  const bool inside_attach = lateral_sign * corner.curvature > 0.0;
  const double kappa = std::abs(corner.curvature) * (inside_attach ? 2.0 : 1.0);
  // Fillet bound: the pavement corners between the branch edges and the
  // target edges must leave a 3 m-radius fillet's tangent leg between the
  // corner and each cut face, or the junction surface has to shrink its
  // corner arcs below the visual floor (tee visual finding, follow-up to
  // issue #103; radius floor mirrors mesh kFilletRadiusFloor). The leg along
  // the target measures from the branch's outer pavement edge; the fillet
  // half-angle at the corner is half the crossing angle on the acute side.
  constexpr double kFilletRadiusFloor = 3.0;
  const double corner_half_angle = std::max(std::min(psi, kPi - psi) / 2.0, 0.1);
  const double fillet_bound = half_width_at(network, *attaching, attach_station) +
                              (kFilletRadiusFloor / std::tan(corner_half_angle)) + 0.5;
  double gap = width_bound;
  for (int round = 0; round < 3; ++round) {
    const double sweep = gap * kappa;
    const double turn_toward_tail = std::min(psi + sweep, kMaxDeflection);
    const double turn_toward_head = std::min(kPi - psi + sweep, kMaxDeflection);
    const double turning_bound = (options.generation.min_turn_radius_m *
                                  std::tan(std::max(turn_toward_tail, turn_toward_head) / 2.0)) +
                                 1.0;
    gap = std::max({width_bound, turning_bound, fillet_bound});
  }
  return gap;
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

  const PathPoint corner = target->plan_view.evaluate(s);
  const double branch_length = attaching->plan_view.length();
  const PathPoint branch_face =
      attaching->plan_view.evaluate(end.contact == ContactPoint::Start ? 0.0 : branch_length);

  const double gap = t_attach_gap(network, end, target_id, s, options);
  const double length = target->plan_view.length();
  if (s - gap <= tol::kLength || s + gap >= length - tol::kLength) {
    return fail("attach point too close to the target road's end — use an endpoint junction "
                "or attach farther from the ends");
  }

  // The branch face needs the same clearance from the corner (the target's
  // reference-line point at s) that the gap gives the target's cut faces —
  // otherwise the turns are cramped against the branch mouth regardless of
  // gap. On a wide target the gap alone can still park the face barely past
  // the target's pavement edge, which pinches the mouth's corner fillets to
  // cosmetic size — so the face also clears the target pavement by the
  // 3 m fillet's tangent leg (mirrors t_attach_gap's fillet bound on the
  // target side). The junction consumes the branch's overhang: trim it back
  // so its face sits at the required chord distance from the corner, exactly
  // as the target loses its middle stub. A branch too short to trim is a
  // user error.
  const double branch_in_hdg_trim =
      end.contact == ContactPoint::Start ? branch_face.hdg + std::numbers::pi : branch_face.hdg;
  const double psi_trim =
      std::abs(std::remainder(branch_in_hdg_trim - corner.hdg, 2.0 * std::numbers::pi));
  const double corner_half_angle_trim =
      std::max(std::min(psi_trim, std::numbers::pi - psi_trim) / 2.0, 0.1);
  constexpr double kFilletRadiusFloorTrim = 3.0; // mirrors mesh kFilletRadiusFloor
  const double fillet_face_distance = half_width_at(network, *target, s) +
                                      (kFilletRadiusFloorTrim / std::tan(corner_half_angle_trim)) +
                                      0.5;
  const double required_face_distance = std::max(gap, fillet_face_distance);
  const double face_distance = std::hypot(branch_face.x - corner.x, branch_face.y - corner.y);
  const double trim = required_face_distance - face_distance;
  if (trim > tol::kLength) {
    const double kept = branch_length - trim;
    if (kept <= tol::kLength) {
      return fail("the attaching road is too short to reach the junction area — draw it "
                  "longer or attach with an explicit smaller gap");
    }
  }

  // Stage state resolved during apply: arena ids are assigned when the
  // splits run, so later builders read them off the mutated network.
  struct Stages {
    RoadId tail;       // [s+gap, …) — the far half, a junction arm
    RoadId middle;     // [s−gap, s+gap) — deleted; the junction area
    RoadId branch_arm; // the branch after any trim (Start-contact trims re-id it)
  };

  auto stages = std::make_shared<Stages>();
  stages->branch_arm = end.road;
  const JunctionGenOptions generation = options.generation;

  std::vector<CompositeCommand::Builder> builders;
  if (trim > tol::kLength) {
    const RoadId branch_id = end.road;
    if (end.contact == ContactPoint::End) {
      // Keep the head [0, length−trim) under the original id; the stub tail
      // is the overhang inside the junction area.
      builders.push_back([branch_id, cut = branch_length - trim](RoadNetwork& net) {
        return split_road(net, branch_id, cut);
      });
      builders.push_back([branch_id](RoadNetwork& net) {
        const Road& head = *net.road(branch_id);
        return delete_road(net, std::get<RoadId>(head.successor->target));
      });
    } else {
      // Start contact: the overhang is the head [0, trim); the surviving
      // piece is the split's new tail — the arm id is resolved at apply.
      builders.push_back(
          [branch_id, cut = trim](RoadNetwork& net) { return split_road(net, branch_id, cut); });
      builders.push_back([branch_id, stages](RoadNetwork& net) {
        const Road& head = *net.road(branch_id);
        stages->branch_arm = std::get<RoadId>(head.successor->target);
        return delete_road(net, branch_id);
      });
    }
  }
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
  builders.push_back([target_id, contact = end.contact, stages, generation](RoadNetwork& net) {
    const std::array<RoadEnd, 3> ends{
        RoadEnd{.road = target_id, .contact = ContactPoint::End},
        RoadEnd{.road = stages->tail, .contact = ContactPoint::Start},
        RoadEnd{.road = stages->branch_arm, .contact = contact},
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

/// Builds ONE connecting road for `cp` into `target`, registering everything it
/// creates in `created`, and returns the connection table entry for it. The
/// single authority for what a connecting road is: shared by
/// materialize_junction (a fresh junction) and regenerate_junction (a turn that
/// appeared on an existing one).
///
/// Holds no reference across a create_* call — every arena insert may
/// reallocate (arena.hpp "never store pointers across mutations").
JunctionConnection materialize_connection(RoadNetwork& target,
                                          Values& created,
                                          JunctionId junction_id,
                                          const ConnectingPlan& cp) {
  const RoadId road_id = target.create_road("", next_free_road_odr_id(target));
  created.roads.emplace_back(road_id, Road{});
  {
    Road& road = *target.road(road_id);
    road.plan_view = cp.line;
    road.length = road.plan_view.length();
    road.elevation = connecting_elevation(cp, road.length);
    road.junction = junction_id;
    // Connecting roads run in driving direction: start touches the incoming
    // arm, end the outgoing arm (12.4.1 laneLink direction rules).
    road.predecessor = RoadLink{.target = cp.from.road, .contact = cp.from.contact};
    road.successor = RoadLink{.target = cp.to.road, .contact = cp.to.contact};
  }

  const LaneSectionId section_id = target.add_lane_section(road_id, 0.0);
  created.sections.emplace_back(section_id, LaneSection{});
  const LaneId center = target.add_lane(section_id, 0, LaneType::None);
  created.lanes.emplace_back(center, Lane{});
  // Single right-hand driving lane carrying the +s (driving-direction) flow.
  const LaneId drive = target.add_lane(section_id, -1, LaneType::Driving);
  created.lanes.emplace_back(drive, Lane{});
  const double length = target.road(road_id)->length;
  Lane& lane = *target.lane(drive);
  lane.widths.push_back(connecting_lane_width(cp, length));
  lane.predecessor = cp.from_lane;
  lane.successor = cp.to_lane;

  return JunctionConnection{.incoming_road = cp.from.road,
                            .connecting_road = road_id,
                            .contact_point = ContactPoint::Start,
                            .lane_links = {{cp.from_lane, -1}}};
}

/// Builds the junction record, its connecting roads and the connection table
/// into `target` from `plan`, registering every created object in `created`;
/// also links each incoming end to the junction. Shared by create_junction's
/// creator (regeneration reuses materialize_connection per turn instead).
/// Assumes the arm link slots are free — validated at factory time.
Expected<void> materialize_junction(RoadNetwork& target,
                                    Values& created,
                                    std::span<const RoadEnd> ends,
                                    const JunctionPlan& plan) {
  const JunctionId junction_id = target.create_junction(next_free_junction_odr_id(target), "");
  created.junctions.emplace_back(junction_id, Junction{});
  target.junction(junction_id)->arms.assign(ends.begin(), ends.end());

  for (const RoadEnd& end : ends) {
    Road& road = *target.road(end.road);
    const RoadLink link{.target = junction_id, .contact = ContactPoint::Start};
    if (end.contact == ContactPoint::Start) {
      road.predecessor = link;
    } else {
      road.successor = link;
    }
  }

  std::vector<JunctionConnection> connections;
  connections.reserve(plan.roads.size());
  for (const ConnectingPlan& cp : plan.roads) {
    connections.push_back(materialize_connection(target, created, junction_id, cp));
  }
  // Re-fetch: every create_road above may have reallocated the road arena, and
  // create_junction the junction arena.
  target.junction(junction_id)->connections = std::move(connections);
  return {};
}

std::unique_ptr<Command> create_junction(const RoadNetwork& network,
                                         std::span<const RoadEnd> ends,
                                         const JunctionGenOptions& options) {
  static constexpr std::string_view kName = "Create Junction";
  // Single-owner invariant (gate finding 5): a road end already claimed by a
  // junction may not become an arm of a second one — regenerate the existing
  // junction instead of overlaying a duplicate. Checked before the link-slot
  // precondition so the message names the owning junction.
  for (const RoadEnd& end : ends) {
    if (const auto owner = junction_at_end(network, end)) {
      const Road* road = network.road(end.road);
      const Junction* junction = network.junction(*owner);
      return invalid_command(
          std::string(kName),
          Error{.code = ErrorCode::InvalidArgument,
                .message = fmt::format(
                    "road '{}' end already belongs to junction {} — regenerate that junction "
                    "instead",
                    road != nullptr ? road->odr_id : "?",
                    junction != nullptr ? junction->odr_id : "?")});
    }
  }
  if (auto free = ends_link_slots_free(network, ends); !free.has_value()) {
    return invalid_command(std::string(kName), free.error());
  }
  auto plan = plan_junction(network, ends, options);
  if (!plan.has_value()) {
    return invalid_command(std::string(kName), plan.error());
  }

  // junctions_are_current: materialize_junction below IS the generator — it
  // builds the connection table and the connecting roads. Regenerating on top
  // of a fresh build would re-plan what was just planned.
  auto command = std::make_unique<GenericCommand>(
      std::string(kName), DirtySet{.topology = true, .junctions_are_current = true});
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

std::unique_ptr<Command> close_gap(const RoadNetwork& network,
                                   const RoadEnd& a,
                                   const RoadEnd& b,
                                   const CloseGapOptions& options) {
  static constexpr std::string_view kName = "Close Gap";
  if (auto ok = check_linkable(network, a, b, options); !ok.has_value()) {
    return invalid_command(std::string(kName), ok.error());
  }
  const ContactState ca = *contact_state(network, a); // valid: check_linkable passed
  const ContactState cb = *contact_state(network, b);
  const double gap = std::hypot(ca.x - cb.x, ca.y - cb.y);

  // Which resolved link slot each end owns.
  const auto slot_of = [](Road& road, ContactPoint contact) -> std::optional<RoadLink>& {
    return contact == ContactPoint::Start ? road.predecessor : road.successor;
  };

  auto command = std::make_unique<GenericCommand>(
      std::string(kName), DirtySet{.roads = {a.road, b.road}, .topology = true});
  command->before.roads.emplace_back(a.road, *network.road(a.road));
  command->before.roads.emplace_back(b.road, *network.road(b.road));

  if (gap <= options.coincident_gap_m) {
    // Near-coincident ends: a pure link weld (Create Road's tangent-continuation
    // snap and "Link Ends" for touching ends). No new geometry.
    Road road_a = *network.road(a.road);
    Road road_b = *network.road(b.road);
    slot_of(road_a, a.contact) = RoadLink{.target = b.road, .contact = b.contact};
    slot_of(road_b, b.contact) = RoadLink{.target = a.road, .contact = a.contact};
    command->after.roads.emplace_back(a.road, std::move(road_a));
    command->after.roads.emplace_back(b.road, std::move(road_b));
    return command;
  }

  // A real gap: bridge it with a single-lane G2 connector road linked at both
  // ends (a → connector → b). The connector leaves a along a's continuation
  // tangent and arrives at b along b's, matching curvature at both joints so an
  // arc starting at either end shows no kink (finding 3). contact_state
  // curvature is signed along into_hdg; the connector's END travels along b's
  // out_hdg (the opposite sense), so b's endpoint curvature is negated.
  auto connector = fit_connector(ConnectorEndpoint{.x = ca.x,
                                                   .y = ca.y,
                                                   .heading = ca.into_hdg,
                                                   .curvature = ca.curvature,
                                                   .z = ca.z,
                                                   .grade = ca.grade},
                                 ConnectorEndpoint{.x = cb.x,
                                                   .y = cb.y,
                                                   .heading = cb.out_hdg,
                                                   .curvature = -cb.curvature,
                                                   .z = cb.z,
                                                   .grade = cb.grade},
                                 ConnectorParams{.g2 = true});
  if (!connector.has_value()) {
    return invalid_command(std::string(kName), connector.error());
  }
  // Width for the connector's single driving lane: the incoming end's outermost
  // driving-lane width, or a default when the end carries none.
  const std::vector<ContactLane> lanes = driving_lanes_at(network, a, ca, /*incoming=*/true);
  const double lane_width = lanes.empty() ? 3.5 : lanes.back().width;

  command->creator = [a,
                      b,
                      line = std::move(connector->line),
                      elevation = std::move(connector->elevation),
                      lane_width](RoadNetwork& target, Values& created) -> Expected<void> {
    const RoadId road_id = target.create_road("", next_free_road_odr_id(target));
    created.roads.emplace_back(road_id, Road{});
    Road& road = *target.road(road_id);
    road.plan_view = line;
    road.length = road.plan_view.length();
    road.elevation = elevation;
    road.predecessor = RoadLink{.target = a.road, .contact = a.contact};
    road.successor = RoadLink{.target = b.road, .contact = b.contact};

    const LaneSectionId section_id = target.add_lane_section(road_id, 0.0);
    created.sections.emplace_back(section_id, LaneSection{});
    const LaneId center = target.add_lane(section_id, 0, LaneType::None);
    created.lanes.emplace_back(center, Lane{});
    const LaneId drive = target.add_lane(section_id, -1, LaneType::Driving);
    created.lanes.emplace_back(drive, Lane{});
    target.lane(drive)->widths.push_back(Poly3{.s = 0.0, .a = lane_width});

    Road& road_a = *target.road(a.road);
    (a.contact == ContactPoint::Start ? road_a.predecessor : road_a.successor) =
        RoadLink{.target = road_id, .contact = ContactPoint::Start};
    Road& road_b = *target.road(b.road);
    (b.contact == ContactPoint::Start ? road_b.predecessor : road_b.successor) =
        RoadLink{.target = road_id, .contact = ContactPoint::End};
    return {};
  };
  return command;
}

std::unique_ptr<Command> create_linked_road(const RoadNetwork& network,
                                            std::vector<Waypoint> waypoints,
                                            LaneProfile profile,
                                            std::string name,
                                            RoadEnd link_start,
                                            EndpointHeadings locked) {
  static constexpr std::string_view kName = "Create Linked Road";
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([waypoints = std::move(waypoints),
                      profile = std::move(profile),
                      name = std::move(name),
                      locked](RoadNetwork& net) {
    (void)net;
    return create_road(waypoints, profile, name, locked);
  });
  builders.push_back(
      [existing = std::move(existing), link_start](RoadNetwork& net) -> std::unique_ptr<Command> {
        RoadId created;
        net.for_each_road([&](RoadId id, const Road&) {
          if (std::ranges::find(existing, id) == existing.end()) {
            created = id;
          }
        });
        const RoadEnd new_start{.road = created, .contact = ContactPoint::Start};
        // Weld only when the two ends really can link; otherwise the road stands
        // on its own (a no-op stage keeps create_linked_road from ever failing
        // the create on the link).
        if (check_linkable(net, new_start, link_start).has_value()) {
          return close_gap(net, new_start, link_start);
        }
        return std::make_unique<GenericCommand>(std::string(kName), DirtySet{});
      });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.topology = true}, std::move(builders));
}

std::unique_ptr<Command> extend_road(const RoadNetwork& network, RoadEnd end, Waypoint to) {
  static constexpr std::string_view kName = "Extend Road";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Road* road = network.road(end.road);
  if (road == nullptr) {
    return fail("stale road id");
  }
  if (end.contact == ContactPoint::Start) {
    return fail("extending from a road's start is out of scope; extend the end");
  }
  if (road->successor.has_value()) {
    return fail("cannot extend a road end that is already linked");
  }
  if (!road->authoring_waypoints.has_value()) {
    return fail("road has no authoring waypoints to extend");
  }
  auto contact = contact_state(network, end);
  if (!contact.has_value()) {
    return invalid_command(std::string(kName), contact.error());
  }
  const ContactState& cs = *contact;
  // Forward clothoid from the end pose (into_hdg is the +s tangent at an END
  // contact) honouring the end curvature — the join is curvature-continuous by
  // construction.
  auto extension =
      fit_forward_clothoid(Waypoint{.x = cs.x, .y = cs.y}, cs.into_hdg, cs.curvature, to);
  if (!extension.has_value()) {
    return invalid_command(std::string(kName), extension.error());
  }

  const double old_length = road->plan_view.length();
  Road after = *road;
  for (const GeometryRecord& record : extension->records()) {
    after.plan_view.append(record); // append() re-derives contiguous s
  }
  after.length = after.plan_view.length();
  const double new_length = after.length;
  after.authoring_waypoints->push_back(to);

  // Continue the vertical profile so BOTH z and grade stay continuous at the
  // join: pin the end node to the contact (z, grade) and extend linearly at the
  // same grade to the new end (set_elevation_profile's C1 Hermite semantics).
  std::vector<ElevationPoint> points = elevation_profile_points(*road);
  if (!points.empty()) {
    points.back().s = old_length;
    points.back().z = cs.z;
    points.back().grade = cs.grade;
  }
  points.push_back(ElevationPoint{
      .s = new_length, .z = cs.z + (cs.grade * (new_length - old_length)), .grade = cs.grade});
  std::vector<double> s_values;
  std::vector<double> z_values;
  std::vector<double> grades;
  s_values.reserve(points.size());
  z_values.reserve(points.size());
  grades.reserve(points.size());
  for (const ElevationPoint& point : points) {
    s_values.push_back(point.s);
    z_values.push_back(point.z);
    grades.push_back(point.grade.value_or(0.0));
  }
  const bool all_zero = std::ranges::all_of(points, [](const ElevationPoint& p) {
    return std::abs(p.z) < 1e-12 && std::abs(p.grade.value_or(0.0)) < 1e-12;
  });
  after.elevation =
      all_zero ? std::vector<Poly3>{} : fit_elevation_profile(s_values, z_values, grades);

  auto command = std::make_unique<GenericCommand>(std::string(kName),
                                                  DirtySet{.roads = {end.road}, .topology = true});
  command->before.roads.emplace_back(end.road, *road);
  command->after.roads.emplace_back(end.road, std::move(after));
  return command;
}

std::unique_ptr<Command> create_teed_road(const RoadNetwork& network,
                                          std::vector<Waypoint> waypoints,
                                          LaneProfile profile,
                                          std::string name,
                                          RoadId target,
                                          double s,
                                          ContactPoint teed_end,
                                          EndpointHeadings locked) {
  static constexpr std::string_view kName = "Create Teed Road";
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([waypoints = std::move(waypoints),
                      profile = std::move(profile),
                      name = std::move(name),
                      locked](RoadNetwork& net) {
    (void)net;
    return create_road(waypoints, profile, name, locked);
  });
  builders.push_back([existing = std::move(existing), target, s, teed_end](
                         RoadNetwork& net) -> std::unique_ptr<Command> {
    RoadId created;
    net.for_each_road([&](RoadId id, const Road&) {
      if (std::ranges::find(existing, id) == existing.end()) {
        created = id;
      }
    });
    return attach_t_junction(net, RoadEnd{.road = created, .contact = teed_end}, target, s);
  });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.topology = true}, std::move(builders));
}

std::unique_ptr<Command> create_crossing_road(const RoadNetwork& network,
                                              std::vector<Waypoint> waypoints,
                                              LaneProfile profile,
                                              std::string name,
                                              RoadId target,
                                              EndpointHeadings locked) {
  static constexpr std::string_view kName = "Create Crossing Road";
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([waypoints = std::move(waypoints),
                      profile = std::move(profile),
                      name = std::move(name),
                      locked](RoadNetwork& net) {
    (void)net;
    return create_road(waypoints, profile, name, locked);
  });
  builders.push_back(
      [existing = std::move(existing), target](RoadNetwork& net) -> std::unique_ptr<Command> {
        RoadId created;
        net.for_each_road([&](RoadId id, const Road&) {
          if (std::ranges::find(existing, id) == existing.end()) {
            created = id;
          }
        });
        return assembly::cross_roads(net, created, target);
      });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.topology = true}, std::move(builders));
}

std::unique_ptr<Command> regenerate_junction(const RoadNetwork& network,
                                             JunctionId junction_id,
                                             const JunctionGenOptions& options,
                                             TurnSetPolicy policy) {
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
  if (policy == TurnSetPolicy::InPlaceOnly && plan->roads.size() != junction->connections.size()) {
    return fail("regeneration changed the connection count; delete and recreate the junction");
  }

  // Match each freshly planned turn to its existing connecting road by KEY —
  // the (incoming road+contact+lane, outgoing road+contact+lane) it links — not
  // by generation order. A node drag can re-order the plan (e.g. a turn crossing
  // the 10-degree left-turn lane-discipline threshold) while the turn SET is
  // unchanged; index matching would then refuse (freezing the junction) or write
  // geometry onto the wrong connecting road (gate finding 2).
  struct TurnKey {
    RoadId from_road;
    ContactPoint from_contact = ContactPoint::Start;
    int from_lane = 0;
    RoadId to_road;
    ContactPoint to_contact = ContactPoint::Start;
    int to_lane = 0;
    bool operator==(const TurnKey&) const = default;
  };

  const auto connection_key = [&](const JunctionConnection& connection) -> std::optional<TurnKey> {
    const Road* road = network.road(connection.connecting_road);
    if (road == nullptr || road->sections.empty() || !road->predecessor.has_value() ||
        !road->successor.has_value() || connection.lane_links.empty()) {
      return std::nullopt;
    }
    const RoadId* from_road = std::get_if<RoadId>(&road->predecessor->target);
    const RoadId* to_road = std::get_if<RoadId>(&road->successor->target);
    if (from_road == nullptr || to_road == nullptr) {
      return std::nullopt;
    }
    int to_lane = 0;
    for (const LaneId lane_id : network.lane_section(road->sections.front())->lanes) {
      const Lane& lane = *network.lane(lane_id);
      if (lane.odr_id == -1 && lane.successor.has_value()) {
        to_lane = *lane.successor;
      }
    }
    return TurnKey{.from_road = *from_road,
                   .from_contact = road->predecessor->contact,
                   .from_lane = connection.lane_links.front().first,
                   .to_road = *to_road,
                   .to_contact = road->successor->contact,
                   .to_lane = to_lane};
  };

  // Partition the plan against the existing table. `matched` pairs a planned
  // turn with the connecting road that already serves it (id reused); what is
  // left over on each side is a turn that appeared or one that vanished.
  struct Matched {
    ConnectingPlan cp;
    RoadId road;
  };

  std::vector<Matched> matched_turns;
  std::vector<ConnectingPlan> new_turns;
  std::vector<bool> claimed(junction->connections.size(), false);
  for (const ConnectingPlan& cp : plan->roads) {
    const TurnKey want{.from_road = cp.from.road,
                       .from_contact = cp.from.contact,
                       .from_lane = cp.from_lane,
                       .to_road = cp.to.road,
                       .to_contact = cp.to.contact,
                       .to_lane = cp.to_lane};
    std::size_t found = junction->connections.size();
    for (std::size_t i = 0; i < junction->connections.size(); ++i) {
      if (claimed[i]) {
        continue;
      }
      if (const auto key = connection_key(junction->connections[i]);
          key.has_value() && *key == want) {
        found = i;
        break;
      }
    }
    if (found == junction->connections.size()) {
      if (policy == TurnSetPolicy::InPlaceOnly) {
        // Same count but a different turn set (e.g. a lane retyped so a turn
        // moved to a different lane): the ids can't be reused in place.
        return fail("regeneration changed the turn set; delete and recreate the junction");
      }
      new_turns.push_back(cp);
      continue;
    }
    claimed[found] = true;
    matched_turns.push_back(
        Matched{.cp = cp, .road = junction->connections[found].connecting_road});
  }

  // Unclaimed connections serve a turn the plan no longer contains. NOTE: a
  // connection whose key could not be read at all (a malformed connecting road
  // — no sections, missing links, empty lane_links) lands here too and is
  // rebuilt from the plan rather than reported. That repairs it, but silently.
  std::vector<RoadId> dropped;
  for (std::size_t i = 0; i < junction->connections.size(); ++i) {
    if (!claimed[i]) {
      dropped.push_back(junction->connections[i].connecting_road);
    }
  }

  // Every connecting road is dirty so incremental re-mesh re-tessellates the
  // survivors and drops the mesh entries of the erased ones.
  DirtySet dirty{.junctions = {junction_id},
                 .topology = !new_turns.empty() || !dropped.empty(),
                 .junctions_are_current = true};
  for (const JunctionConnection& connection : junction->connections) {
    dirty.roads.push_back(connection.connecting_road);
  }
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));

  // The junction record itself changes (the connection table is rewritten), so
  // it belongs in `before` — the creator mutates it in the network and
  // GenericCommand re-reads `after` from there once the creator has run.
  command->before.junctions.emplace_back(junction_id, *junction);
  for (const Matched& match : matched_turns) {
    const Road* road = network.road(match.road);
    command->before.roads.emplace_back(match.road, *road);
    for (const LaneId lane_id : network.lane_section(road->sections.front())->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane->odr_id == -1) {
        command->before.lanes.emplace_back(lane_id, *lane);
      }
    }
  }
  capture_road_erasure(network, *command, dropped);

  command->creator = [junction_id,
                      matched_turns = std::move(matched_turns),
                      new_turns = std::move(new_turns),
                      dropped](RoadNetwork& target, Values& created) -> Expected<void> {
    // Pass 1: rewrite the turns that survive. No creation happens here, so
    // references stay valid within an iteration.
    std::vector<JunctionConnection> table;
    table.reserve(matched_turns.size() + new_turns.size());
    for (const Matched& match : matched_turns) {
      Road& road = *target.road(match.road);
      road.plan_view = match.cp.line;
      road.length = road.plan_view.length();
      road.elevation = connecting_elevation(match.cp, road.length);
      const double length = road.length;
      for (const LaneId lane_id : target.lane_section(road.sections.front())->lanes) {
        Lane& lane = *target.lane(lane_id);
        if (lane.odr_id == -1) {
          lane.widths = {connecting_lane_width(match.cp, length)};
        }
      }
      table.push_back(JunctionConnection{.incoming_road = match.cp.from.road,
                                         .connecting_road = match.road,
                                         .contact_point = ContactPoint::Start,
                                         .lane_links = {{match.cp.from_lane, -1}}});
    }
    // Pass 2: build the turns that appeared. Every create_* here may realloc
    // an arena, which is why no reference from pass 1 survives into it.
    for (const ConnectingPlan& cp : new_turns) {
      table.push_back(materialize_connection(target, created, junction_id, cp));
    }
    // The dropped roads are erased by `erased` after this returns; dropping
    // them from the table first means they are never referenced when they go.
    target.junction(junction_id)->connections = std::move(table);
    return {};
  };
  return command;
}

std::unique_ptr<Command> move_waypoint_following_junctions(const RoadNetwork& network,
                                                           RoadId road_id,
                                                           std::size_t index,
                                                           Waypoint to) {
  // Only junctions that can actually regenerate: one with no recorded arms came
  // from a foreign file, and regenerate_junction calls that an error. Skipping
  // it keeps the drag alive (the junction stays stale, exactly as it does on
  // the commit path) instead of refusing every frame.
  std::vector<JunctionId> followed;
  for (const JunctionId junction_id : junctions_touching(network, road_id)) {
    const Junction* junction = network.junction(junction_id);
    if (junction != nullptr && !junction->arms.empty()) {
      followed.push_back(junction_id);
    }
  }
  if (followed.empty()) {
    return move_waypoint(network, road_id, index, to);
  }

  // Validate here rather than inside a stage: a refused move should come back
  // as the same invalid_command the plain factory returns, not as a composite
  // that fails on apply.
  auto probe = move_waypoint(network, road_id, index, to);
  if (probe == nullptr) {
    return nullptr;
  }

  std::vector<CompositeCommand::Builder> builders;
  builders.reserve(followed.size() + 1);
  builders.push_back(
      [road_id, index, to](RoadNetwork& net) { return move_waypoint(net, road_id, index, to); });
  for (const JunctionId junction_id : followed) {
    // Built lazily, so each regeneration plans against the network with the
    // move already applied — the whole point of following mid-drag.
    //
    // InPlaceOnly: this command is a preview factory, rebuilt and discarded on
    // every drag frame. A regeneration that created connecting roads would have
    // them erase_exact'd by the frame's revert and then lose the only handle
    // that could restore those slots when the command is destroyed — reserving
    // arena slots, per frame, for the rest of the session. A drag that changes
    // the turn set therefore leaves the junction stale (and toasts) exactly as
    // it does today; lane edits, which are not previewed, regenerate normally.
    builders.push_back([junction_id](RoadNetwork& net) {
      return regenerate_junction(net, junction_id, {}, TurnSetPolicy::InPlaceOnly);
    });
  }
  // Same undo-menu text as the plain move: whether the drag happened to touch a
  // junction is not something the user should read in the Edit menu.
  return std::make_unique<CompositeCommand>(
      std::string(probe->name()), DirtySet{}, std::move(builders));
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

  // junctions_are_current: the junction named here is the one being deleted,
  // and capture_deletion strips the doomed connections from every survivor —
  // there is nothing left to regenerate against.
  DirtySet dirty{
      .roads = doomed, .junctions = {junction_id}, .topology = true, .junctions_are_current = true};
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

// --- parametric intersection assemblies -------------------------------------

namespace assembly {

namespace {

/// Widest side's total lane width [m]; the junction area must clear it.
double profile_half_width(const LaneProfile& profile) {
  const auto side_width = [](const std::vector<LaneSpec>& lanes) {
    double sum = 0.0;
    for (const LaneSpec& lane : lanes) {
      sum += lane.width;
    }
    return sum;
  };
  return std::max(side_width(profile.left), side_width(profile.right));
}

double resolve_gap(const IntersectionParams& params) {
  if (params.gap_m > 0.0) {
    return params.gap_m;
  }
  // Match attach_t_junction's spirit: the area must clear the pavement and
  // give the tightest turn room. tan(45°)=1 for the 90° arms, so the turn
  // bound is ~min_turn_radius.
  return std::max(profile_half_width(params.profile) + 1.0,
                  params.generation.min_turn_radius_m + 1.0);
}

/// The junction half-span for teeing/crossing a stem of `params.profile` onto
/// `target` at `s`: the wider of the two pavements plus clearance, and room for
/// the tightest turn — so the stem's inner end and the target's cut faces meet
/// at the same boundary and attach_t_junction (given this gap) produces a clean
/// perpendicular tee.
double onto_road_gap(const RoadNetwork& network,
                     RoadId target,
                     double s,
                     const IntersectionParams& params) {
  if (params.gap_m > 0.0) {
    return params.gap_m;
  }
  const Road* road = network.road(target);
  const double target_half = road != nullptr ? half_width_at(network, *road, s) : 0.0;
  const double branch_half = profile_half_width(params.profile);
  return std::max(std::max(target_half, branch_half) + 1.0,
                  params.generation.min_turn_radius_m + 1.0);
}

/// Composite for an N-arm intersection whose arm directions are
/// pose.heading + offsets[i]. Each arm is a straight stub authored from the
/// junction boundary (gap out from the center) to gap+arm_length, inner→outer
/// so its Start faces the junction; the final builder links every stub into a
/// generated common junction. One command (apply→revert byte-identical).
std::unique_ptr<Command> make_intersection(const RoadNetwork& network,
                                           std::string_view name,
                                           Pose pose,
                                           std::span<const double> offsets,
                                           const IntersectionParams& params) {
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(name),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (params.arm_length_m <= tol::kLength) {
    return fail("intersection arm length must be positive");
  }
  if (params.profile.left.empty() && params.profile.right.empty()) {
    return fail("intersection lane profile is empty");
  }

  const double gap = resolve_gap(params);
  const double outer = gap + params.arm_length_m;

  // Roads present now; every road the stub builders create (absent here) is
  // an arm — the junction builder discovers them the way attach_t_junction
  // discovers its split-created ids.
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });

  std::vector<CompositeCommand::Builder> builders;
  for (const double offset : offsets) {
    const double angle = pose.heading + offset;
    const double dx = std::cos(angle);
    const double dy = std::sin(angle);
    std::vector<Waypoint> waypoints{
        Waypoint{.x = pose.x + (gap * dx), .y = pose.y + (gap * dy)},
        Waypoint{.x = pose.x + (outer * dx), .y = pose.y + (outer * dy)},
    };
    builders.push_back([waypoints = std::move(waypoints), profile = params.profile, angle](
                           RoadNetwork& net) {
      (void)net;
      return create_road(waypoints, profile, {}, EndpointHeadings{.start = angle, .end = angle});
    });
  }

  builders.push_back(
      [existing = std::move(existing), generation = params.generation](RoadNetwork& net) {
        std::vector<RoadEnd> ends;
        net.for_each_road([&](RoadId id, const Road&) {
          if (std::ranges::find(existing, id) == existing.end()) {
            ends.push_back(RoadEnd{.road = id, .contact = ContactPoint::Start});
          }
        });
        return create_junction(net, ends, generation);
      });

  return std::make_unique<CompositeCommand>(
      std::string(name), DirtySet{.topology = true}, std::move(builders));
}

} // namespace

std::unique_ptr<Command>
t_intersection(const RoadNetwork& network, Pose pose, IntersectionParams params) {
  // Through road along pose.heading (arms at 0 and π) plus a perpendicular
  // stem to the left (+π/2).
  const std::array<double, 3> offsets{0.0, std::numbers::pi, std::numbers::pi / 2.0};
  return make_intersection(network, "T-Intersection", pose, offsets, params);
}

std::unique_ptr<Command>
x_intersection(const RoadNetwork& network, Pose pose, IntersectionParams params) {
  const std::array<double, 4> offsets{
      0.0, std::numbers::pi / 2.0, std::numbers::pi, 3.0 * std::numbers::pi / 2.0};
  return make_intersection(network, "X-Intersection", pose, offsets, params);
}

std::unique_ptr<Command>
tee_onto_road(const RoadNetwork& network, RoadId target, double s, IntersectionParams params) {
  static constexpr std::string_view kName = "Tee onto Road";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (params.arm_length_m <= tol::kLength) {
    return fail("intersection arm length must be positive");
  }
  const Road* target_road = network.road(target);
  if (target_road == nullptr) {
    return fail("stale target road id");
  }
  // Project + align (finding 1): a perpendicular stem leaving the road's left
  // side at s, instead of a floating standalone junction at the cursor.
  auto pose = aligned_pose_on_road(network, target, s, Side::Left);
  if (!pose.has_value()) {
    return invalid_command(std::string(kName), pose.error());
  }
  const double gap = onto_road_gap(network, target, s, params);
  if (s - gap <= tol::kLength || s + gap >= target_road->plan_view.length() - tol::kLength) {
    return fail("the drop is too near a road end to fit a junction — drop it further along");
  }
  const double angle = pose->heading; // road tangent + 90°, pointing off the road
  const double dx = std::cos(angle);
  const double dy = std::sin(angle);
  std::vector<Waypoint> stem{
      Waypoint{.x = pose->x + (gap * dx), .y = pose->y + (gap * dy)},
      Waypoint{.x = pose->x + ((gap + params.arm_length_m) * dx),
               .y = pose->y + ((gap + params.arm_length_m) * dy)},
  };
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([stem = std::move(stem), profile = params.profile, angle](RoadNetwork& net) {
    (void)net;
    return create_road(stem, profile, {}, EndpointHeadings{.start = angle, .end = angle});
  });
  builders.push_back(
      [existing = std::move(existing), target, s, gap, generation = params.generation](
          RoadNetwork& net) -> std::unique_ptr<Command> {
        RoadId stem_id;
        net.for_each_road([&](RoadId id, const Road&) {
          if (std::ranges::find(existing, id) == existing.end()) {
            stem_id = id;
          }
        });
        return attach_t_junction(net,
                                 RoadEnd{.road = stem_id, .contact = ContactPoint::Start},
                                 target,
                                 s,
                                 TAttachOptions{.gap_m = gap, .generation = generation});
      });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.topology = true}, std::move(builders));
}

std::unique_ptr<Command>
cross_onto_road(const RoadNetwork& network, RoadId target, double s, IntersectionParams params) {
  static constexpr std::string_view kName = "Cross onto Road";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (params.arm_length_m <= tol::kLength) {
    return fail("intersection arm length must be positive");
  }
  const Road* target_road = network.road(target);
  if (target_road == nullptr) {
    return fail("stale target road id");
  }
  auto pose_left = aligned_pose_on_road(network, target, s, Side::Left);
  auto pose_right = aligned_pose_on_road(network, target, s, Side::Right);
  if (!pose_left.has_value()) {
    return invalid_command(std::string(kName), pose_left.error());
  }
  const double gap = onto_road_gap(network, target, s, params);
  if (s - gap <= tol::kLength || s + gap >= target_road->plan_view.length() - tol::kLength) {
    return fail("the drop is too near a road end to fit a junction — drop it further along");
  }
  // Two collinear through arms (the split halves of the target) plus two
  // perpendicular stems make the 4-way. The middle [s−gap, s+gap] becomes the
  // junction area, exactly as attach_t_junction removes its stub.
  const auto stem_waypoints = [&](const Pose2D& pose) {
    const double dx = std::cos(pose.heading);
    const double dy = std::sin(pose.heading);
    return std::vector<Waypoint>{Waypoint{.x = pose.x + (gap * dx), .y = pose.y + (gap * dy)},
                                 Waypoint{.x = pose.x + ((gap + params.arm_length_m) * dx),
                                          .y = pose.y + ((gap + params.arm_length_m) * dy)}};
  };

  struct Stages {
    RoadId stem_left;
    RoadId stem_right;
    RoadId tail;
    RoadId middle;
  };

  auto stages = std::make_shared<Stages>();
  std::vector<RoadId> existing;
  network.for_each_road([&](RoadId id, const Road&) { existing.push_back(id); });
  const double angle_left = pose_left->heading;
  const double angle_right = pose_right->heading;

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([wp = stem_waypoints(*pose_left), profile = params.profile, angle_left](
                         RoadNetwork& net) {
    (void)net;
    return create_road(wp, profile, {}, EndpointHeadings{.start = angle_left, .end = angle_left});
  });
  builders.push_back(
      [existing, wp = stem_waypoints(*pose_right), profile = params.profile, angle_right, stages](
          RoadNetwork& net) {
        net.for_each_road([&](RoadId id, const Road&) {
          if (std::ranges::find(existing, id) == existing.end()) {
            stages->stem_left = id;
          }
        });
        return create_road(
            wp, profile, {}, EndpointHeadings{.start = angle_right, .end = angle_right});
      });
  builders.push_back([existing, stages, target, cut = s + gap](RoadNetwork& net) {
    net.for_each_road([&](RoadId id, const Road&) {
      if (std::ranges::find(existing, id) == existing.end() && id != stages->stem_left) {
        stages->stem_right = id;
      }
    });
    return split_road(net, target, cut);
  });
  builders.push_back([target, cut = s - gap, stages](RoadNetwork& net) {
    stages->tail = std::get<RoadId>(net.road(target)->successor->target);
    return split_road(net, target, cut);
  });
  builders.push_back([target, stages](RoadNetwork& net) {
    stages->middle = std::get<RoadId>(net.road(target)->successor->target);
    return delete_road(net, stages->middle);
  });
  builders.push_back([target, stages, generation = params.generation](RoadNetwork& net) {
    const std::array<RoadEnd, 4> ends{
        RoadEnd{.road = target, .contact = ContactPoint::End},
        RoadEnd{.road = stages->tail, .contact = ContactPoint::Start},
        RoadEnd{.road = stages->stem_left, .contact = ContactPoint::Start},
        RoadEnd{.road = stages->stem_right, .contact = ContactPoint::Start},
    };
    return create_junction(net, ends, generation);
  });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.topology = true}, std::move(builders));
}

std::unique_ptr<Command>
cross_roads(const RoadNetwork& network, RoadId a, RoadId b, IntersectionParams params) {
  static constexpr std::string_view kName = "Cross Roads";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Road* road_a = network.road(a);
  const Road* road_b = network.road(b);
  if (road_a == nullptr || road_b == nullptr) {
    return fail("stale road id");
  }
  if (a == b) {
    return fail("a road cannot cross itself");
  }
  if (road_a->junction.is_valid() || road_b->junction.is_valid()) {
    return fail("a road already inside a junction cannot be crossed");
  }

  auto crossings = road_intersections(network, a, b);
  if (!crossings.has_value()) {
    return invalid_command(std::string(kName), crossings.error());
  }
  // The first crossing whose junction area (station ± gap) fits strictly inside
  // BOTH roads. gap follows onto_road_gap: clear the wider pavement and give the
  // tightest turn room.
  const double length_a = road_a->plan_view.length();
  const double length_b = road_b->plan_view.length();
  std::optional<RoadCrossing> chosen;
  double gap = 0.0;
  for (const RoadCrossing& crossing : *crossings) {
    const double g = params.gap_m > 0.0
                         ? params.gap_m
                         : std::max(std::max(half_width_at(network, *road_a, crossing.s_a),
                                             half_width_at(network, *road_b, crossing.s_b)) +
                                        1.0,
                                    params.generation.min_turn_radius_m + 1.0);
    if (crossing.s_a - g > tol::kLength && crossing.s_a + g < length_a - tol::kLength &&
        crossing.s_b - g > tol::kLength && crossing.s_b + g < length_b - tol::kLength) {
      chosen = crossing;
      gap = g;
      break;
    }
  }
  if (!chosen.has_value()) {
    return fail("the roads do not cross with room for a junction — no interior crossing, or one "
                "too near a road end");
  }

  // Split each road at its crossing station ± gap and remove the middle stub;
  // the four surviving ends form the 4-way junction. Split-created ids are read
  // off the mutated network at apply, exactly as cross_onto_road does.
  struct Stages {
    RoadId a_tail;
    RoadId b_tail;
    RoadId a_middle;
    RoadId b_middle;
  };

  auto stages = std::make_shared<Stages>();
  const double s_a = chosen->s_a;
  const double s_b = chosen->s_b;

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back([a, cut = s_a + gap](RoadNetwork& net) { return split_road(net, a, cut); });
  builders.push_back([a, cut = s_a - gap, stages](RoadNetwork& net) {
    stages->a_tail = std::get<RoadId>(net.road(a)->successor->target);
    return split_road(net, a, cut);
  });
  builders.push_back([a, stages](RoadNetwork& net) {
    stages->a_middle = std::get<RoadId>(net.road(a)->successor->target);
    return delete_road(net, stages->a_middle);
  });
  builders.push_back([b, cut = s_b + gap](RoadNetwork& net) { return split_road(net, b, cut); });
  builders.push_back([b, cut = s_b - gap, stages](RoadNetwork& net) {
    stages->b_tail = std::get<RoadId>(net.road(b)->successor->target);
    return split_road(net, b, cut);
  });
  builders.push_back([b, stages](RoadNetwork& net) {
    stages->b_middle = std::get<RoadId>(net.road(b)->successor->target);
    return delete_road(net, stages->b_middle);
  });
  builders.push_back([a, b, stages, generation = params.generation](RoadNetwork& net) {
    const std::array<RoadEnd, 4> ends{
        RoadEnd{.road = a, .contact = ContactPoint::End},
        RoadEnd{.road = stages->a_tail, .contact = ContactPoint::Start},
        RoadEnd{.road = b, .contact = ContactPoint::End},
        RoadEnd{.road = stages->b_tail, .contact = ContactPoint::Start},
    };
    return create_junction(net, ends, generation);
  });
  return std::make_unique<CompositeCommand>(
      std::string(kName), DirtySet{.roads = {a, b}, .topology = true}, std::move(builders));
}

} // namespace assembly

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

  // A lane on an end section changes what driving_lanes_at reports, and so the
  // junction's turn set: name the junctions so the editor regenerates them.
  // A lane on an interior section leaves the turn set alone and regeneration
  // is a byte-identical no-op — cheaper than deciding here which sections are
  // ends.
  auto command = std::make_unique<GenericCommand>(
      std::string(kName),
      DirtySet{.roads = {section->road},
               .junctions = junctions_touching(network, section->road),
               .topology = true});
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

  // Every junction the road touches, not just those whose lane_links this
  // command pruned: losing a lane drops a turn from the plan even where no
  // lane_link named it, and regeneration is what rebuilds the turn set.
  DirtySet dirty{.roads = {context->road_id},
                 .junctions = junctions_touching(network, context->road_id),
                 .topology = true};
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

std::unique_ptr<Command>
insert_lane(const RoadNetwork& network, LaneSectionId section_id, int at_odr_id, LaneType type) {
  static constexpr std::string_view kName = "Insert Lane";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return fail("stale lane-section id");
  }
  if (at_odr_id == 0) {
    return fail("cannot insert at the center lane");
  }
  const int side = at_odr_id > 0 ? 1 : -1;
  bool occupied = false;
  for (const LaneId lane_id : section->lanes) {
    if (network.lane(lane_id)->odr_id == at_odr_id) {
      occupied = true;
      break;
    }
  }
  if (!occupied) {
    // Numbering stays contiguous; appending past the outermost is add_lane.
    return fail("no lane at that position to insert before; use add_lane to append");
  }

  // Everything at or outside the insert point on this side steps one further
  // out. `new = old + side` for both sides (more negative on the right, more
  // positive on the left), and since it is a contiguous outer block the shift
  // preserves the section's descending lane order.
  const auto shifted = [&](int odr) {
    return odr != 0 && (side > 0 ? odr >= at_odr_id : odr <= at_odr_id);
  };

  const RoadId road_id = section->road;
  DirtySet dirty{
      .roads = {road_id}, .junctions = junctions_touching(network, road_id), .topology = true};
  auto command = std::make_unique<GenericCommand>(std::string(kName), std::move(dirty));

  // `before` snapshots everything the creator mutates so the engine can revert
  // it and re-read `after` from the network. The section's lane list gains the
  // new lane; the shifted lanes change odr id.
  command->before.sections.emplace_back(section_id, *section);
  for (const LaneId lane_id : section->lanes) {
    if (shifted(network.lane(lane_id)->odr_id)) {
      command->before.lanes.emplace_back(lane_id, *network.lane(lane_id));
    }
  }

  // Adjacent-section links that named a shifted lane by id are remapped, not
  // cleared — the lanes still continue, just under new numbers. The writer
  // refuses a dangling intra-road link in either direction.
  const Road& road = *network.road(road_id);
  const auto here = std::ranges::find(road.sections, section_id);
  const auto capture_neighbor = [&](LaneSectionId neighbor_id, bool forward) {
    for (const LaneId neighbor_lane_id : network.lane_section(neighbor_id)->lanes) {
      const Lane& lane = *network.lane(neighbor_lane_id);
      const std::optional<int>& link = forward ? lane.successor : lane.predecessor;
      if (link.has_value() && shifted(*link)) {
        command->before.lanes.emplace_back(neighbor_lane_id, lane);
      }
    }
  };
  if (here != road.sections.begin()) {
    capture_neighbor(*std::prev(here), /*forward=*/true);
  }
  if (here != road.sections.end() && std::next(here) != road.sections.end()) {
    capture_neighbor(*std::next(here), /*forward=*/false);
  }

  // Junction lane_links that named a shifted lane by id are remapped too.
  std::vector<JunctionId> touched_junctions;
  network.for_each_junction([&](JunctionId junction_id, const Junction& junction) {
    const bool touched = std::ranges::any_of(junction.connections, [&](const auto& connection) {
      return std::ranges::any_of(connection.lane_links, [&](const std::pair<int, int>& link) {
        return (connection.incoming_road == road_id && shifted(link.first)) ||
               (connection.connecting_road == road_id && shifted(link.second));
      });
    });
    if (touched) {
      command->before.junctions.emplace_back(junction_id, junction);
      touched_junctions.push_back(junction_id);
    }
  });

  command->creator = [section_id,
                      at_odr_id,
                      side,
                      type,
                      road_id,
                      touched_junctions = std::move(touched_junctions)](
                         RoadNetwork& target, Values& created) -> Expected<void> {
    const auto shift = [&](int odr) {
      return odr != 0 && (side > 0 ? odr >= at_odr_id : odr <= at_odr_id) ? odr + side : odr;
    };
    // 1. Renumber the outer block. Mutating odr id in place keeps the section's
    // descending order because the block is contiguous.
    for (const LaneId lane_id : target.lane_section(section_id)->lanes) {
      Lane& lane = *target.lane(lane_id);
      lane.odr_id = shift(lane.odr_id);
    }
    // 2. Add the new lane at the now-free position. It appears mid-road, so it
    // is not linked back to either neighbouring section.
    const LaneId lane_id = target.add_lane(section_id, at_odr_id, type);
    if (!lane_id.is_valid()) {
      return make_error(ErrorCode::InvalidArgument, "insert position is still occupied");
    }
    target.lane(lane_id)->widths = {Poly3{.a = 3.5}};
    created.lanes.emplace_back(lane_id, Lane{});
    // 3. Remap every link that named a shifted lane by id.
    const Road& owner = *target.road(road_id);
    const auto pos = std::ranges::find(owner.sections, section_id);
    const auto remap_neighbor = [&](LaneSectionId neighbor_id, bool forward) {
      for (const LaneId neighbor_lane_id : target.lane_section(neighbor_id)->lanes) {
        Lane& neighbor = *target.lane(neighbor_lane_id);
        std::optional<int>& link = forward ? neighbor.successor : neighbor.predecessor;
        if (link.has_value()) {
          *link = shift(*link);
        }
      }
    };
    if (pos != owner.sections.begin()) {
      remap_neighbor(*std::prev(pos), /*forward=*/true);
    }
    if (pos != owner.sections.end() && std::next(pos) != owner.sections.end()) {
      remap_neighbor(*std::next(pos), /*forward=*/false);
    }
    for (const JunctionId junction_id : touched_junctions) {
      for (JunctionConnection& connection : target.junction(junction_id)->connections) {
        for (std::pair<int, int>& link : connection.lane_links) {
          if (connection.incoming_road == road_id) {
            link.first = shift(link.first);
          }
          if (connection.connecting_road == road_id) {
            link.second = shift(link.second);
          }
        }
      }
    }
    return {};
  };
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
  // Retyping to or from Driving changes what driving_lanes_at reports, so a
  // turn appears, disappears, or moves to a different lane — name the
  // junctions so regeneration rebuilds the turn set.
  auto command = std::make_unique<GenericCommand>(
      std::string(kName),
      DirtySet{.roads = {context->road_id},
               .junctions = junctions_touching(network, context->road_id)});
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
  // Refuse to flatten a width that varies along s. This op used to overwrite
  // `widths` unconditionally, so a single constant-width edit silently
  // destroyed every taper on the lane — including one the user had just
  // carved, and any authored by a foreign .xodr.
  const std::vector<Poly3>& widths = context->lane.widths;
  const bool constant = widths.size() <= 1 &&
                        (widths.empty() || (widths.front().b == 0.0 && widths.front().c == 0.0 &&
                                            widths.front().d == 0.0));
  if (!constant) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument,
                                 .message = "this lane's width varies along s; use "
                                            "set_lane_width_profile to edit it"});
  }
  Lane after = context->lane;
  after.widths = {Poly3{.a = width_m}};
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {context->road_id}});
  command->before.lanes.emplace_back(lane_id, context->lane);
  command->after.lanes.emplace_back(lane_id, std::move(after));
  return command;
}

std::unique_ptr<Command>
set_lane_width_profile(const RoadNetwork& network, LaneId lane_id, std::vector<Poly3> widths) {
  static constexpr std::string_view kName = "Set Lane Width Profile";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  auto context = lane_context(network, lane_id);
  if (!context.has_value()) {
    return invalid_command(std::string(kName), context.error());
  }
  // asam.net:xodr:1.4.0:road.lane.center_lane_no_width
  if (context->lane.odr_id == 0) {
    return fail("the center lane has no width");
  }
  if (widths.empty()) {
    return fail("width profile must have at least one record");
  }
  // asam.net:xodr:1.7.0:road.lane.width.width_defined_whole_section — the
  // width must cover the whole section, so a record at sOffset 0 must exist.
  if (std::abs(widths.front().s) > tol::kLength) {
    return fail("width profile must start with a record at sOffset 0");
  }
  for (std::size_t i = 0; i < widths.size(); ++i) {
    // asam.net:xodr:1.4.0:road.lane.width.lane_width_validity — width shall
    // be >= 0. Zero is legal and load-bearing: a turn lane tapers up from 0.
    if (widths[i].a < 0.0) {
      return fail("width must be >= 0 at every record start");
    }
    // asam.net:xodr:1.4.0:road.lane.width.elem_asc_order
    if (i > 0 && widths[i].s <= widths[i - 1].s + tol::kLength) {
      return fail("width records must ascend by sOffset");
    }
  }
  auto span = section_end(network, context->section_id);
  if (!span.has_value()) {
    return invalid_command(std::string(kName), span.error());
  }
  const double length = *span - network.lane_section(context->section_id)->s0;
  if (widths.back().s >= length - tol::kLength) {
    return fail("every width record must start inside the lane section");
  }

  Lane after = context->lane;
  after.widths = std::move(widths);
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {context->road_id}});
  command->before.lanes.emplace_back(lane_id, context->lane);
  command->after.lanes.emplace_back(lane_id, std::move(after));
  return command;
}

std::unique_ptr<Command> split_lane_section(const RoadNetwork& network, RoadId road_id, double s) {
  static constexpr std::string_view kName = "Split Lane Section";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail("stale road id");
  }
  if (road->sections.empty()) {
    return fail("road has no lane sections");
  }
  if (s <= tol::kLength || s >= road->plan_view.length() - tol::kLength) {
    return fail("split station must lie strictly inside the road");
  }
  const LaneSectionId covering_id = section_at(network, road_id, s);
  if (!covering_id.is_valid()) {
    return fail("no lane section covers the split station");
  }
  const LaneSection& covering = *network.lane_section(covering_id);

  // Idempotent at an existing boundary: Carve cuts both ends of a span and
  // must not have to special-case a taper that starts where a section does.
  // An empty command applies and reverts as a no-op, keeping the
  // byte-identical contract trivially.
  if (std::abs(covering.s0 - s) <= tol::kLength) {
    return std::make_unique<GenericCommand>(std::string(kName), DirtySet{});
  }
  const double local = s - covering.s0;

  // The copy that takes [s, end). Captured now, replayed by the creator.
  struct LaneCopy {
    int odr_id = 0;
    LaneType type = LaneType::None;
    std::vector<Poly3> widths;
    std::vector<RoadMark> marks;
    std::optional<int> predecessor; // unset when the lane does not continue
    std::optional<int> successor;   // the original's successor moves here
  };

  std::vector<LaneCopy> copies;
  copies.reserve(covering.lanes.size());

  auto command = std::make_unique<GenericCommand>(std::string(kName),
                                                  DirtySet{.roads = {road_id}, .topology = true});
  command->before.roads.emplace_back(road_id, *road);

  // The originals keep [s0, s); `after` is recomputed from the network once a
  // creator runs, so the creator writes these rather than command->after.
  std::vector<std::pair<LaneId, Lane>> truncated_originals;
  truncated_originals.reserve(covering.lanes.size());

  for (const LaneId lane_id : covering.lanes) {
    const Lane& lane = *network.lane(lane_id);
    // A lane continues across the seam only if it is physically there on both
    // sides (§11.6: "Both lanes have a non-zero width at the connection
    // point"). The center lane always continues — it carries no width by
    // rule, so evaluating its (empty) profile would wrongly say zero.
    const bool continues = lane.odr_id == 0 || eval_profile(lane.widths, local) > tol::kLength;
    // asam.net:xodr:1.4.0:road.lane.link.lanes_across_laneSections — a
    // continuing lane is connected in BOTH directions. The ids are identical
    // across the seam, so the next section's predecessors already name the
    // copy correctly and need no rewrite.
    const std::optional<int> seam = continues ? std::optional<int>{lane.odr_id} : std::nullopt;
    copies.push_back(LaneCopy{.odr_id = lane.odr_id,
                              .type = lane.type,
                              .widths = rebase_profile(lane.widths, local),
                              .marks = rebase_marks(lane.road_marks, local),
                              .predecessor = seam,
                              .successor = lane.successor});

    Lane truncated = lane;
    truncated.widths = truncate_profile(lane.widths, local);
    truncated.road_marks = truncate_marks(lane.road_marks, local);
    truncated.successor = seam;
    command->before.lanes.emplace_back(lane_id, lane);
    truncated_originals.emplace_back(lane_id, std::move(truncated));
  }

  command->creator = [road_id,
                      s,
                      copies = std::move(copies),
                      truncated_originals = std::move(truncated_originals)](
                         RoadNetwork& target, Values& created) -> Expected<void> {
    const LaneSectionId new_section = target.add_lane_section(road_id, s);
    if (!new_section.is_valid()) {
      return make_error(ErrorCode::InvalidArgument,
                        "a lane section already starts at this station");
    }
    for (const LaneCopy& copy : copies) {
      const LaneId new_lane = target.add_lane(new_section, copy.odr_id, copy.type);
      if (!new_lane.is_valid()) {
        return make_error(ErrorCode::InvalidArgument,
                          "lane id already occupied in the new section");
      }
      Lane& value = *target.lane(new_lane);
      value.widths = copy.widths;
      value.road_marks = copy.marks;
      value.predecessor = copy.predecessor;
      value.successor = copy.successor;
      created.lanes.emplace_back(new_lane, Lane{});
    }
    for (const auto& [lane_id, value] : truncated_originals) {
      *target.lane(lane_id) = value;
    }
    created.sections.emplace_back(new_section, LaneSection{});
    return {};
  };
  return command;
}

namespace {

// The taper length each ramp spans, in metres (p2-s5). Long enough to read as
// a lane change rather than a spike; clamped down when the section is short.
constexpr double kTaperLen = 15.0;

// Section-local width records for a lane that ramps up from zero to `W` over
// the first `t` metres of a section of length `L`. When `ramp_down` is true
// (Lane Add pocket) it plateaus, then ramps back to zero over the last `t`
// metres — width zero at BOTH seams, so the lane needs no cross-section link.
// When false (Lane Form) it holds `W` to the section end.
//
// A record at sOffset 0 is always present and records ascend, matching the
// set_lane_width_profile contract; the caller checks back().s < L - tol.
std::vector<Poly3> taper_records(double L, double W, double t, bool ramp_down) {
  const double slope = t > tol::kLength ? W / t : 0.0;
  std::vector<Poly3> records;
  records.push_back(Poly3{.s = 0.0, .a = 0.0, .b = slope}); // 0 -> W over [0, t]
  if (ramp_down) {
    // A plateau only where the two ramps do not already meet (L > 2t); a
    // shorter span is a triangle straight from up-ramp to down-ramp.
    if (L - 2.0 * t > tol::kLength) {
      records.push_back(Poly3{.s = t, .a = W});
    }
    records.push_back(Poly3{.s = L - t, .a = W, .b = -slope}); // W -> 0 over [L-t, L]
  } else {
    records.push_back(Poly3{.s = t, .a = W}); // hold W to the section end
  }
  return records;
}

// The outermost lane on `side` in `section` (the most extreme odr id), or an
// invalid id when the side carries no lane. add_lane appends beyond it and
// insert_lane's fresh lane becomes it, so this is how a composite stage finds
// the lane a previous stage just produced.
LaneId outermost_lane_on_side(const RoadNetwork& network, LaneSectionId section_id, int side) {
  LaneId found{};
  int extreme = 0;
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return found;
  }
  for (const LaneId lane_id : section->lanes) {
    const int odr = network.lane(lane_id)->odr_id;
    if (side > 0 ? odr > extreme : odr < extreme) {
      extreme = odr;
      found = lane_id;
    }
  }
  return found;
}

LaneId lane_with_odr(const RoadNetwork& network, LaneSectionId section_id, int odr_id) {
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return LaneId{};
  }
  for (const LaneId lane_id : section->lanes) {
    if (network.lane(lane_id)->odr_id == odr_id) {
      return lane_id;
    }
  }
  return LaneId{};
}

// The width of the driving lane on `side` nearest to `exclude` (the freshly
// added pocket lane), or 3.5 m when the side carries no other driving lane.
// add_lane copies the literal outermost lane — which is often a shoulder — so a
// pocket meant as a travel lane takes a real driving-lane width instead.
double driving_lane_width_on_side(const RoadNetwork& network,
                                  LaneSectionId section_id,
                                  int side,
                                  LaneId exclude) {
  const LaneSection* section = network.lane_section(section_id);
  if (section == nullptr) {
    return 3.5;
  }
  const Lane* nearest = nullptr;
  for (const LaneId lane_id : section->lanes) {
    if (lane_id == exclude) {
      continue;
    }
    const Lane* lane = network.lane(lane_id);
    const bool on_side = side > 0 ? lane->odr_id > 0 : lane->odr_id < 0;
    if (!on_side || lane->type != LaneType::Driving) {
      continue;
    }
    // Prefer the outermost driving lane — the one the pocket sits against.
    if (nearest == nullptr ||
        (side > 0 ? lane->odr_id > nearest->odr_id : lane->odr_id < nearest->odr_id)) {
      nearest = lane;
    }
  }
  if (nearest == nullptr || nearest->widths.empty()) {
    return 3.5;
  }
  return nearest->widths.front().a;
}

} // namespace

std::unique_ptr<Command> add_lane_span(
    const RoadNetwork& network, RoadId road_id, int side, double s0, double s1, LaneType type) {
  static constexpr std::string_view kName = "Add Lane Span";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (side != 1 && side != -1) {
    return fail("side must be +1 (left) or -1 (right)");
  }
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail("stale road id");
  }
  if (s0 >= s1 - tol::kLength) {
    return fail("lane span must have s0 < s1");
  }
  const double length = road->plan_view.length();
  // Clamp the span inward so a seam never abuts a road end: splits refuse a
  // station within tol of the ends, and — the load-bearing part — a pocket
  // that stays strictly interior needs no cross-section links at all.
  constexpr double kEndInset = 0.5;
  const double lo = std::max(s0, kEndInset);
  const double hi = std::min(s1, length - kEndInset);
  if (hi - lo <= tol::kLength) {
    return fail("lane span collapses once clamped inside the road");
  }

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back(
      [road_id, lo](RoadNetwork& net) { return split_lane_section(net, road_id, lo); });
  builders.push_back(
      [road_id, hi](RoadNetwork& net) { return split_lane_section(net, road_id, hi); });
  builders.push_back([road_id, lo, side, type](RoadNetwork& net) -> std::unique_ptr<Command> {
    const LaneSectionId mid = section_at(net, road_id, lo + tol::kLength);
    return add_lane(net, mid, side, type);
  });
  builders.push_back([road_id, lo, side](RoadNetwork& net) -> std::unique_ptr<Command> {
    const LaneSectionId mid = section_at(net, road_id, lo + tol::kLength);
    const LaneId lane = outermost_lane_on_side(net, mid, side);
    if (!lane.is_valid()) {
      return invalid_command(
          std::string(kName),
          Error{.code = ErrorCode::InvalidArgument,
                .message = "the pocket lane vanished before it could be shaped"});
    }
    auto span = section_end(net, mid);
    if (!span.has_value()) {
      return invalid_command(std::string(kName), span.error());
    }
    const double L = *span - net.lane_section(mid)->s0;
    // The pocket is a travel lane: shape it to the nearest driving lane's width
    // (3.5 m default), not add_lane's copy of the outermost lane, which is often
    // a narrow shoulder.
    const double W = driving_lane_width_on_side(net, mid, side, lane);
    const double t = std::min(kTaperLen, L / 2.0 - tol::kLength);
    return set_lane_width_profile(net, lane, taper_records(L, W, t, /*ramp_down=*/true));
  });

  // SEED the touched junctions so an arm regenerates even for an interior
  // section, and leave junctions_are_current false so the editor's regen loop
  // still runs (add_lane on an interior section is a byte-identical no-op).
  DirtySet base{
      .roads = {road_id}, .junctions = junctions_touching(network, road_id), .topology = true};
  return std::make_unique<CompositeCommand>(
      std::string(kName), std::move(base), std::move(builders));
}

std::unique_ptr<Command> form_lane(const RoadNetwork& network,
                                   RoadId road_id,
                                   int side,
                                   double s_start,
                                   int at_odr_id,
                                   LaneType type) {
  static constexpr std::string_view kName = "Form Lane";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  if (side != 1 && side != -1) {
    return fail("side must be +1 (left) or -1 (right)");
  }
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail("stale road id");
  }
  const double length = road->plan_view.length();
  if (s_start <= tol::kLength || s_start >= length - tol::kLength) {
    return fail("form station must lie strictly inside the road");
  }
  if ((at_odr_id > 0 ? 1 : -1) != side || at_odr_id == 0) {
    return fail("at_odr_id must be non-zero and share the sign of side");
  }

  std::vector<CompositeCommand::Builder> builders;
  builders.push_back(
      [road_id, s_start](RoadNetwork& net) { return split_lane_section(net, road_id, s_start); });
  builders.push_back(
      [road_id, s_start, at_odr_id, type](RoadNetwork& net) -> std::unique_ptr<Command> {
        const LaneSectionId target = section_at(net, road_id, s_start + tol::kLength);
        // Forward-linking a formed lane across a downstream seam is out of
        // scope (p2-s5): only form into the FINAL section, where the lane can
        // run to the road terminus with nothing downstream to link.
        if (target != net.road(road_id)->sections.back()) {
          return invalid_command(std::string(kName),
                                 Error{.code = ErrorCode::InvalidArgument,
                                       .message =
                                           "Lane Form reaches a downstream lane-section boundary; "
                                           "form nearer the road end"});
        }
        return insert_lane(net, target, at_odr_id, type);
      });
  builders.push_back([road_id, s_start, at_odr_id](RoadNetwork& net) -> std::unique_ptr<Command> {
    const LaneSectionId target = section_at(net, road_id, s_start + tol::kLength);
    const LaneId lane = lane_with_odr(net, target, at_odr_id);
    if (!lane.is_valid()) {
      return invalid_command(
          std::string(kName),
          Error{.code = ErrorCode::InvalidArgument,
                .message = "the formed lane vanished before it could be shaped"});
    }
    auto span = section_end(net, target);
    if (!span.has_value()) {
      return invalid_command(std::string(kName), span.error());
    }
    const double L = *span - net.lane_section(target)->s0;
    constexpr double W = 3.5; // insert_lane gives the fresh lane width 3.5
    const double t = std::min(kTaperLen, L - tol::kLength);
    return set_lane_width_profile(net, lane, taper_records(L, W, t, /*ramp_down=*/false));
  });

  DirtySet base{
      .roads = {road_id}, .junctions = junctions_touching(network, road_id), .topology = true};
  return std::make_unique<CompositeCommand>(
      std::string(kName), std::move(base), std::move(builders));
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

std::vector<ElevationPoint> elevation_profile_points(const Road& road) {
  std::vector<ElevationPoint> points;
  const double length = road.plan_view.length();
  if (road.elevation.empty()) {
    points.push_back(ElevationPoint{.s = 0.0, .z = 0.0, .grade = 0.0});
    points.push_back(ElevationPoint{.s = length, .z = 0.0, .grade = 0.0});
    return points;
  }
  for (const Poly3& record : road.elevation) {
    points.push_back(ElevationPoint{
        .s = record.s, .z = record.eval(record.s), .grade = record.eval_derivative(record.s)});
  }
  const Poly3& last = road.elevation.back();
  if (length - last.s > tol::kLength) {
    points.push_back(
        ElevationPoint{.s = length, .z = last.eval(length), .grade = last.eval_derivative(length)});
  }
  return points;
}

std::unique_ptr<Command> set_elevation_profile(const RoadNetwork& network,
                                               RoadId road_id,
                                               std::vector<ElevationPoint> points) {
  static constexpr std::string_view kName = "Edit Elevation Profile";
  const auto fail = [&](std::string message) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = std::move(message)});
  };
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return fail("stale road id");
  }
  if (points.empty()) {
    return fail("an elevation profile needs at least one node");
  }
  std::ranges::sort(points, {}, &ElevationPoint::s);
  const double length = road->plan_view.length();
  for (std::size_t i = 0; i < points.size(); ++i) {
    if (points[i].s < -tol::kLength || points[i].s > length + tol::kLength) {
      return fail("profile node station outside the road");
    }
    if (i > 0 && points[i].s - points[i - 1].s <= tol::kLength) {
      return fail("duplicate profile node stations");
    }
  }

  // Fill missing grades by finite differences (the M2 node-elevation
  // behavior), then fit the C1 Hermite through the explicit set.
  std::vector<double> s_values;
  std::vector<double> z_values;
  s_values.reserve(points.size());
  z_values.reserve(points.size());
  for (const ElevationPoint& point : points) {
    s_values.push_back(point.s);
    z_values.push_back(point.z);
  }
  std::vector<double> grades(points.size(), 0.0);
  const std::size_t n = points.size();
  for (std::size_t i = 0; i < n; ++i) {
    if (points[i].grade.has_value()) {
      grades[i] = *points[i].grade;
    } else if (n == 1) {
      grades[i] = 0.0;
    } else if (i == 0) {
      grades[i] = (z_values[1] - z_values[0]) / (s_values[1] - s_values[0]);
    } else if (i + 1 == n) {
      grades[i] = (z_values[n - 1] - z_values[n - 2]) / (s_values[n - 1] - s_values[n - 2]);
    } else {
      grades[i] = (z_values[i + 1] - z_values[i - 1]) / (s_values[i + 1] - s_values[i - 1]);
    }
  }

  const bool all_zero = std::ranges::all_of(points, [](const ElevationPoint& p) {
    return std::abs(p.z) < 1e-12 && std::abs(p.grade.value_or(0.0)) < 1e-12;
  });

  Road after = *road;
  after.elevation =
      all_zero ? std::vector<Poly3>{} : fit_elevation_profile(s_values, z_values, grades);

  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.roads = {road_id}});
  command->before.roads.emplace_back(road_id, *road);
  command->after.roads.emplace_back(road_id, std::move(after));
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

// ---- objects (road props) ---------------------------------------------------

Error stale_object_error() {
  return Error{.code = ErrorCode::InvalidArgument, .message = "stale object id"};
}

Error object_s_error() {
  return Error{.code = ErrorCode::InvalidArgument, .message = "object s is outside the road"};
}

std::unique_ptr<Command> add_object(const RoadNetwork& network, RoadId road, Object object) {
  static constexpr std::string_view kName = "Add Object";
  const Road* owner = network.road(road);
  if (owner == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  if (object.s < -tol::kLength || object.s > owner->plan_view.length() + tol::kLength) {
    return invalid_command(std::string(kName), object_s_error());
  }
  object.road = road;
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {road}});
  command->creator = [road, object](RoadNetwork& net, Values& created) -> Expected<void> {
    const ObjectId id = net.add_object(road, object);
    if (!id.is_valid()) {
      return make_error(ErrorCode::InvalidArgument, "failed to add object (stale road)");
    }
    created.objects.emplace_back(id, object);
    return {};
  };
  return command;
}

std::unique_ptr<Command> delete_object(const RoadNetwork& network, ObjectId object) {
  static constexpr std::string_view kName = "Delete Object";
  const Object* value = network.object(object);
  if (value == nullptr) {
    return invalid_command(std::string(kName), stale_object_error());
  }
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {value->road}});
  command->erased.objects.emplace_back(object, *value);
  return command;
}

std::unique_ptr<Command> move_object(
    const RoadNetwork& network, ObjectId object, double s, double t, std::optional<double> hdg) {
  static constexpr std::string_view kName = "Move Object";
  const Object* current = network.object(object);
  if (current == nullptr) {
    return invalid_command(std::string(kName), stale_object_error());
  }
  const Road* owner = network.road(current->road);
  if (owner == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument,
                                 .message = "object has a stale road back-reference"});
  }
  if (s < -tol::kLength || s > owner->plan_view.length() + tol::kLength) {
    return invalid_command(std::string(kName), object_s_error());
  }
  Object moved = *current;
  moved.s = s;
  moved.t = t;
  if (hdg.has_value()) {
    moved.hdg = *hdg;
  }
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {current->road}});
  command->before.objects.emplace_back(object, *current);
  command->after.objects.emplace_back(object, std::move(moved));
  return command;
}

// ---- signals (traffic control) ----------------------------------------------

Error stale_signal_error() {
  return Error{.code = ErrorCode::InvalidArgument, .message = "stale signal id"};
}

Error signal_s_error() {
  return Error{.code = ErrorCode::InvalidArgument, .message = "signal s is outside the road"};
}

std::unique_ptr<Command> add_signal(const RoadNetwork& network, RoadId road, Signal signal) {
  static constexpr std::string_view kName = "Add Signal";
  const Road* owner = network.road(road);
  if (owner == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale road id"});
  }
  if (signal.s < -tol::kLength || signal.s > owner->plan_view.length() + tol::kLength) {
    return invalid_command(std::string(kName), signal_s_error());
  }
  signal.road = road;
  auto command = std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {road}});
  command->creator = [road, signal](RoadNetwork& net, Values& created) -> Expected<void> {
    const SignalId id = net.add_signal(road, signal);
    if (!id.is_valid()) {
      return make_error(ErrorCode::InvalidArgument, "failed to add signal (stale road)");
    }
    created.signals.emplace_back(id, signal);
    return {};
  };
  return command;
}

std::unique_ptr<Command> delete_signal(const RoadNetwork& network, SignalId signal) {
  static constexpr std::string_view kName = "Delete Signal";
  const Signal* value = network.signal(signal);
  if (value == nullptr) {
    return invalid_command(std::string(kName), stale_signal_error());
  }
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {value->road}});
  command->erased.signals.emplace_back(signal, *value);
  return command;
}

std::unique_ptr<Command> move_signal(const RoadNetwork& network,
                                     SignalId signal,
                                     double s,
                                     double t,
                                     std::optional<double> h_offset) {
  static constexpr std::string_view kName = "Move Signal";
  const Signal* current = network.signal(signal);
  if (current == nullptr) {
    return invalid_command(std::string(kName), stale_signal_error());
  }
  const Road* owner = network.road(current->road);
  if (owner == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument,
                                 .message = "signal has a stale road back-reference"});
  }
  if (s < -tol::kLength || s > owner->plan_view.length() + tol::kLength) {
    return invalid_command(std::string(kName), signal_s_error());
  }
  Signal moved = *current;
  moved.s = s;
  moved.t = t;
  if (h_offset.has_value()) {
    moved.h_offset = *h_offset;
  }
  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {current->road}});
  command->before.signals.emplace_back(signal, *current);
  command->after.signals.emplace_back(signal, std::move(moved));
  return command;
}

std::unique_ptr<Command>
set_object_model(const RoadNetwork& network, ObjectId object, std::string model_id) {
  static constexpr std::string_view kName = "Set Object Model";
  const Object* current = network.object(object);
  if (current == nullptr) {
    return invalid_command(std::string(kName),
                           Error{.code = ErrorCode::InvalidArgument, .message = "stale object id"});
  }
  if (model_id.empty()) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "empty prop model id"});
  }
  const props::PropModel* model = props::model(model_id);
  if (model == nullptr) {
    return invalid_command(
        std::string(kName),
        Error{.code = ErrorCode::InvalidArgument, .message = "unknown prop model id: " + model_id});
  }

  Object retargeted = *current;
  retargeted.name = std::move(model_id);
  // The bounding volume describes the model, so it must travel with it —
  // otherwise a pine's radius would survive on a shrub.
  retargeted.radius = model->radius;
  retargeted.height = model->height;

  auto command =
      std::make_unique<GenericCommand>(std::string(kName), DirtySet{.objects = {current->road}});
  command->before.objects.emplace_back(object, *current);
  command->after.objects.emplace_back(object, std::move(retargeted));
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
