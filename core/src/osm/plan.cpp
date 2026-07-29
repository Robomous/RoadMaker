/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "roadmaker/geometry/simplify.hpp"
#include "roadmaker/osm/network_plan.hpp"
#include "roadmaker/osm/tags.hpp"
#include "roadmaker/road/defaults.hpp"
#include "roadmaker/xodr/rules.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numbers>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace roadmaker::osm {
namespace {

using defaults::RoadClass;

/// A node identity for topology purposes. **`layer` is part of the key**, and
/// that is the whole overpass rule: two ways sharing an OSM node at different
/// layers are a bridge and the road beneath it, so they must not meet. Without
/// it the import silently welds them — invisible in plan view, wrong in every
/// 3D consumer downstream.
struct NodeKey {
  OsmId node = 0;
  int layer = 0;
  // Ordered so it can key a std::map. Only the ordering is used, so no
  // equality operator is declared -- an unused one is a -Werror failure.
  friend auto operator<=>(const NodeKey&, const NodeKey&) = default;
};

/// Emits diagnostics up to a cap, then aggregates by reason with counts.
class DiagnosticSink {
public:
  DiagnosticSink(std::vector<Diagnostic>& into, std::string source)
      : into_(into), source_(std::move(source)) {}

  void say(Severity severity, std::string_view rule, std::string location, std::string message) {
    if (named_ < kMaxNamedDiagnostics) {
      ++named_;
      into_.push_back(Diagnostic{.severity = severity,
                                 .location = std::move(location),
                                 .message = std::move(message),
                                 .rule_id = std::string(rule)});
      return;
    }
    ++suppressed_[std::string(rule)];
  }

  /// One summary line per reason for everything past the cap. A district can
  /// produce tens of thousands of compromises, and thirty thousand rows
  /// communicate no better than none.
  void flush() {
    for (const auto& [rule, count] : suppressed_) {
      into_.push_back(Diagnostic{
          .severity = Severity::Info,
          .location = source_,
          .message = fmt::format("and {} further occurrence(s) of this diagnostic", count),
          .rule_id = rule});
    }
    suppressed_.clear();
  }

  [[nodiscard]] const std::string& source() const { return source_; }

private:
  std::vector<Diagnostic>& into_;
  std::string source_;
  std::size_t named_ = 0;
  std::map<std::string, std::size_t> suppressed_;
};

std::string way_location(OsmId way) {
  return fmt::format("way/{}", way);
}

std::string segment_location(OsmId way, std::size_t segment) {
  return fmt::format("way/{}/segment/{}", way, segment);
}

/// The cross section a classified way authors.
///
/// Adds and removes WHOLE LANES from the class template and never invents a
/// width: every metre still comes from `roadmaker::defaults`, which is what
/// keeps this clear of test_defaults_registry.cpp's no-private-width-table
/// gate.
LaneProfile profile_for(const HighwayMapping& mapping, bool one_way, std::optional<int> lanes) {
  const RoadClass road_class = mapping.road_class.value_or(RoadClass::Local);
  LaneProfile profile = [road_class] {
    switch (road_class) {
    case RoadClass::Freeway:
      return LaneProfile::freeway();
    case RoadClass::Arterial:
      return LaneProfile::arterial();
    case RoadClass::Collector:
      return LaneProfile::collector();
    case RoadClass::Local:
      return LaneProfile::local_road();
    }
    return LaneProfile::local_road();
  }();

  const double driving_width = defaults::driving_lane_width(road_class);

  const auto count_driving = [](const std::vector<LaneSpec>& side) {
    return static_cast<int>(std::ranges::count(side, LaneType::Driving, &LaneSpec::type));
  };
  // Resize a side's DRIVING lanes to `wanted`, leaving its shoulders,
  // sidewalks and other furniture exactly where the template put them.
  const auto set_driving = [driving_width](std::vector<LaneSpec>& side, int wanted) {
    std::vector<LaneSpec> rebuilt;
    int placed = 0;
    for (const LaneSpec& lane : side) {
      if (lane.type != LaneType::Driving) {
        rebuilt.push_back(lane);
        continue;
      }
      if (placed < wanted) {
        rebuilt.push_back(lane);
        ++placed;
      }
    }
    // Extra driving lanes go innermost, which is where a widened road grows.
    while (placed < wanted) {
      rebuilt.insert(rebuilt.begin(), LaneSpec{.type = LaneType::Driving, .width = driving_width});
      ++placed;
    }
    side = std::move(rebuilt);
  };

  // A ramp built to its parent's full cross section is wrong everywhere.
  if (mapping.link) {
    set_driving(profile.left, 1);
    set_driving(profile.right, 1);
  }

  if (lanes) {
    if (one_way) {
      set_driving(profile.right, std::min(*lanes, kMaxLanesPerSide));
    } else {
      // Remainder to the forward side, which is where an odd count usually
      // means a passing or turning lane.
      const int right = std::min((*lanes + 1) / 2, kMaxLanesPerSide);
      const int left = std::min(*lanes / 2, kMaxLanesPerSide);
      set_driving(profile.right, right);
      set_driving(profile.left, std::max(left, 1));
    }
  }

  if (one_way) {
    // Every driving lane on one side, and no centre line: there is no
    // opposing direction for it to separate.
    std::erase_if(profile.left,
                  [](const LaneSpec& lane) { return lane.type == LaneType::Driving; });
    profile.center_marking = false;
    if (count_driving(profile.right) == 0) {
      set_driving(profile.right, 1);
    }
  }
  return profile;
}

/// Pulls `waypoints` back from one end by `setback` metres, keeping the shape
/// of what remains and landing the new end exactly on the old polyline.
///
/// Needed because OSM states topology by SHARING A NODE: every arm of a
/// crossing runs to the same point, and a connecting road between two arms
/// that meet at a point has zero length and nowhere to curve. Trimming is what
/// gives `create_junction` room to work.
///
/// Returns false (leaving `waypoints` untouched) when the segment is too short
/// to give the metres up — better a coincident joint that reports its dropped
/// turns than a road that vanished.
bool trim_end(std::vector<Waypoint>& waypoints, bool from_start, double setback) {
  if (waypoints.size() < 2 || !(setback > 0.0)) {
    return false;
  }
  double total = 0.0;
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    total += std::hypot(waypoints[i].x - waypoints[i - 1].x, waypoints[i].y - waypoints[i - 1].y);
  }
  if (total < kMinTrimmedSegmentM || total <= setback * 2.0) {
    return false;
  }

  if (from_start) {
    std::ranges::reverse(waypoints);
  }
  // Walk in from the (now) back, consuming `setback`.
  double walked = 0.0;
  while (waypoints.size() > 2) {
    const Waypoint& last = waypoints.back();
    const Waypoint& previous = waypoints[waypoints.size() - 2];
    const double step = std::hypot(last.x - previous.x, last.y - previous.y);
    if (walked + step > setback) {
      const double remaining = setback - walked;
      const double t = step > 0.0 ? remaining / step : 0.0;
      waypoints.back() = Waypoint{.x = last.x + ((previous.x - last.x) * t),
                                  .y = last.y + ((previous.y - last.y) * t)};
      break;
    }
    walked += step;
    waypoints.pop_back();
  }
  if (waypoints.size() == 2) {
    // Two points left: shorten the final leg directly rather than dropping it.
    const Waypoint& last = waypoints.back();
    const Waypoint& previous = waypoints.front();
    const double step = std::hypot(last.x - previous.x, last.y - previous.y);
    const double remaining = std::max(0.0, setback - walked);
    if (step > remaining && step > 0.0) {
      const double t = remaining / step;
      waypoints.back() = Waypoint{.x = last.x + ((previous.x - last.x) * t),
                                  .y = last.y + ((previous.y - last.y) * t)};
    }
  }
  if (from_start) {
    std::ranges::reverse(waypoints);
  }
  return true;
}

/// The turn at `b` between `a`->`b` and `b`->`c`, in radians. Pi is a full
/// reversal.
double turn_angle(const Point2& a, const Point2& b, const Point2& c) {
  const double in = std::atan2(b[1] - a[1], b[0] - a[0]);
  const double out = std::atan2(c[1] - b[1], c[0] - b[0]);
  double delta =
      std::fmod(out - in + (3.0 * std::numbers::pi), 2.0 * std::numbers::pi) - std::numbers::pi;
  return std::abs(delta);
}

} // namespace

double NetworkPlan::area_km2() const {
  const double width = bounds[2] - bounds[0];
  const double height = bounds[3] - bounds[1];
  if (!(width > 0.0) || !(height > 0.0)) {
    return 0.0;
  }
  return (width * height) / 1'000'000.0;
}

Expected<NetworkPlanResult> plan_network(const OsmGraph& graph,
                                         const gis::CrsTransform& transform,
                                         const OsmBuildOptions& options,
                                         std::span<const std::string> existing_odr_ids) {
  NetworkPlanResult result;
  NetworkPlan& plan = result.plan;
  DiagnosticSink diag(result.diagnostics, "osm");

  const std::unordered_set<std::string> existing(existing_odr_ids.begin(), existing_odr_ids.end());

  // --- 1. reproject every node ONCE ---------------------------------------
  // An OSM node is referenced by many ways and the Krüger series is not free,
  // so this is per NODE rather than per reference.
  std::unordered_map<OsmId, Point2> world;
  world.reserve(graph.nodes.size());
  for (const auto& [id, node] : graph.nodes) {
    const auto xy = transform.apply(node.lon_deg, node.lat_deg);
    world.emplace(id, Point2{xy[0], xy[1]});
  }

  // --- 2. classify, and say what is dropped -------------------------------
  struct Kept {
    const OsmWay* way = nullptr;
    const HighwayMapping* mapping = nullptr;
    OneWay one_way;
    std::optional<int> lanes;
    std::optional<MaxSpeed> speed;
    int layer = 0;
  };

  std::vector<Kept> kept;
  kept.reserve(graph.ways.size());

  std::map<std::string, std::size_t> ignored_tag_counts;

  for (const OsmWay& way : graph.ways) {
    const std::string_view highway = way.tag("highway");
    const std::string_view area = way.tag("area");
    const std::string_view access = way.tag("access");

    if (area == "yes" || way.has_tag("area:highway")) {
      ++plan.dropped_ways;
      diag.say(Severity::Warning,
               rules::kOsmElementDropped,
               way_location(way.id),
               "way is an area outline, not a centreline; this build imports centrelines");
      continue;
    }
    if (access == "no" || access == "private") {
      ++plan.dropped_ways;
      diag.say(Severity::Warning,
               rules::kOsmElementDropped,
               way_location(way.id),
               fmt::format("way is tagged access={}", access));
      continue;
    }
    if (is_dropped_highway(highway)) {
      ++plan.dropped_ways;
      diag.say(Severity::Warning,
               rules::kOsmElementDropped,
               way_location(way.id),
               fmt::format("highway={} is not a road classification this build imports", highway));
      continue;
    }
    const HighwayMapping* mapping = highway_mapping(highway);
    if (mapping == nullptr) {
      ++plan.dropped_ways;
      diag.say(Severity::Warning,
               rules::kOsmElementDropped,
               way_location(way.id),
               fmt::format("highway={} is not in this build's mapping table", highway));
      continue;
    }
    if (highway == "service" && !options.include_service_roads) {
      ++plan.dropped_ways;
      diag.say(Severity::Info,
               rules::kOsmElementDropped,
               way_location(way.id),
               "service roads are not imported by default");
      continue;
    }
    if (way.refs.size() < 2) {
      ++plan.dropped_ways;
      diag.say(Severity::Warning,
               rules::kOsmElementDropped,
               way_location(way.id),
               "way has fewer than two usable nodes");
      continue;
    }

    Kept entry;
    entry.way = &way;
    entry.mapping = mapping;
    entry.layer = parse_layer(way.tag("layer"));

    entry.one_way = parse_oneway(way.tag("oneway"));
    const std::string_view junction = way.tag("junction");
    const bool roundabout = junction == "roundabout" || junction == "circular";
    if (roundabout || mapping->implies_oneway) {
      entry.one_way.one_way = true;
    }
    if (roundabout) {
      diag.say(Severity::Warning,
               rules::kOsmFitApproximated,
               way_location(way.id),
               "roundabout imported as one-way segments with a junction at each arm, not as a "
               "single roundabout entity (see #495)");
    }

    if (way.has_tag("lanes")) {
      entry.lanes = parse_lane_count(way.tag("lanes"));
      if (!entry.lanes) {
        diag.say(Severity::Warning,
                 rules::kOsmElementDropped,
                 way_location(way.id),
                 fmt::format("lanes={} is not a plain count; using the road class default",
                             way.tag("lanes")));
      }
    }
    if (way.has_tag("maxspeed")) {
      entry.speed = parse_maxspeed(way.tag("maxspeed"));
      if (!entry.speed) {
        diag.say(
            Severity::Warning,
            rules::kOsmElementDropped,
            way_location(way.id),
            fmt::format("maxspeed={} is not a speed this build can express", way.tag("maxspeed")));
      }
    }
    if (entry.layer != 0 || way.has_tag("bridge") || way.has_tag("tunnel")) {
      diag.say(Severity::Warning,
               rules::kOsmAtGrade,
               way_location(way.id),
               fmt::format("imported at grade: layer={} and its vertical separation is not "
                           "authored (see #496)",
                           entry.layer));
    }

    for (const TagMapping& row : tag_mappings()) {
      if (row.use == TagUse::Dropped && way.has_tag(row.key)) {
        ++ignored_tag_counts[std::string(row.key)];
      }
    }
    kept.push_back(entry);
  }

  for (const auto& [key, count] : ignored_tag_counts) {
    diag.say(Severity::Info,
             rules::kOsmElementDropped,
             "osm",
             fmt::format("tag '{}' is read but not modelled ({} way(s) carry it)", key, count));
  }

  // --- 3. degree over the KEPT ways only, keyed by (node, layer) -----------
  // One O(total refs) pass. A node shared only with a dropped footway is not a
  // junction, which is why this counts kept ways rather than the file's.
  std::map<NodeKey, std::size_t> ref_count;
  for (const Kept& entry : kept) {
    for (const OsmId ref : entry.way->refs) {
      ++ref_count[NodeKey{.node = ref, .layer = entry.layer}];
    }
  }

  // --- 4. split at shared nodes, THEN simplify ----------------------------
  // The order is load-bearing: a junction node is then always a segment
  // ENDPOINT, and the simplifier always keeps endpoints, so a junction can
  // never be simplified away. The alternative needs a protected-vertex set
  // threaded through RDP and gets it wrong at exactly the nodes that matter.
  std::map<NodeKey, std::vector<std::pair<std::size_t, ContactPoint>>> joints;

  for (const Kept& entry : kept) {
    const OsmWay& way = *entry.way;

    std::vector<std::vector<OsmId>> pieces;
    std::vector<OsmId> current{way.refs.front()};
    for (std::size_t i = 1; i < way.refs.size(); ++i) {
      current.push_back(way.refs[i]);
      const bool interior = i + 1 < way.refs.size();
      const bool shared = ref_count[NodeKey{.node = way.refs[i], .layer = entry.layer}] > 1;
      if (interior && shared) {
        pieces.push_back(current);
        current = {way.refs[i]};
      }
    }
    if (current.size() >= 2) {
      pieces.push_back(std::move(current));
    }

    for (std::size_t segment = 0; segment < pieces.size(); ++segment) {
      std::vector<OsmId>& refs = pieces[segment];
      if (entry.one_way.reversed) {
        // -1 reverses the POLYLINE so the reference line runs with traffic.
        // Encoding a reversed lane direction instead is equally valid
        // OpenDRIVE and wrong for every station-anchored thing a user later
        // places: @s would increase against travel.
        std::ranges::reverse(refs);
      }

      std::vector<Point2> points;
      points.reserve(refs.size());
      for (const OsmId ref : refs) {
        if (const auto found = world.find(ref); found != world.end()) {
          points.push_back(found->second);
        }
      }
      if (points.size() < 2) {
        continue;
      }

      FitCompromise compromise;
      compromise.source_nodes = points.size();

      const auto after_collapse = drop_near_duplicates(points, options.min_waypoint_spacing_m);
      compromise.merged_nodes = points.size() - after_collapse.size();
      std::vector<Point2> collapsed;
      collapsed.reserve(after_collapse.size());
      for (const std::size_t index : after_collapse) {
        collapsed.push_back(points[index]);
      }
      if (collapsed.size() < 2) {
        ++plan.dropped_ways;
        diag.say(Severity::Warning,
                 rules::kOsmFitApproximated,
                 segment_location(way.id, segment),
                 "segment collapses to a single point at the minimum waypoint spacing");
        continue;
      }

      // Bounded doubling: over the waypoint cap, ask for less precision and
      // retry, then report the tolerance actually used. Deterministic, and
      // always reported — the #243 guardrail discipline.
      double tolerance = options.simplify_tolerance_m;
      std::vector<std::size_t> retained = simplify_polyline(collapsed, tolerance);
      for (int doubling = 0;
           retained.size() > options.max_waypoints_per_road && doubling < kMaxToleranceDoublings;
           ++doubling) {
        tolerance *= 2.0;
        retained = simplify_polyline(collapsed, tolerance);
      }
      compromise.tolerance_used_m = tolerance;
      compromise.kept_nodes = retained.size();
      compromise.max_deviation_m = polyline_deviation(collapsed, retained);

      std::vector<Waypoint> waypoints;
      waypoints.reserve(retained.size());
      for (const std::size_t index : retained) {
        waypoints.push_back(Waypoint{.x = collapsed[index][0], .y = collapsed[index][1]});
      }

      // A turn a G1 clothoid fit cannot take. In OSM a hairpin is usually a
      // switchback, so splitting there is the honest repair rather than a
      // refusal — but it IS a compromise and is reported as one.
      for (std::size_t i = 1; i + 1 < waypoints.size(); ++i) {
        const Point2 a{waypoints[i - 1].x, waypoints[i - 1].y};
        const Point2 b{waypoints[i].x, waypoints[i].y};
        const Point2 c{waypoints[i + 1].x, waypoints[i + 1].y};
        if (turn_angle(a, b, c) > kHairpinTurnRad) {
          compromise.split_at_hairpin = true;
          break;
        }
      }

      PlannedRoad road;
      road.way = way.id;
      road.segment = segment;
      road.odr_id = fmt::format("osm.{}.{}", way.id, segment);
      road.name = std::string(way.tag("name"));
      road.waypoints = std::move(waypoints);
      road.profile = profile_for(*entry.mapping, entry.one_way.one_way, entry.lanes);
      road.layer = entry.layer;
      road.bridge = way.tag("bridge") == "yes";
      road.tunnel = way.tag("tunnel") == "yes";
      road.compromise = compromise;
      road.start_node = refs.front();
      road.end_node = refs.back();

      RoadTypeRecord type;
      type.s = 0.0;
      type.type = defaults::road_type_name(entry.mapping->road_class.value_or(RoadClass::Local));
      if (entry.speed) {
        // A way whose source records no limit gets a type and NO <speed>:
        // inventing one would be guessing where silence is the honest answer.
        type.speed = RoadSpeed{.max_str = entry.speed->max, .unit = entry.speed->unit};
      }
      road.type = std::move(type);

      if (existing.contains(road.odr_id)) {
        ++plan.skipped_existing;
        continue;
      }
      if (plan.roads.size() >= options.max_roads) {
        diag.say(Severity::Error,
                 rules::kOsmElementDropped,
                 segment_location(way.id, segment),
                 fmt::format("plan reached this build's {} road limit; the rest of the extract "
                             "was not imported",
                             options.max_roads));
        break;
      }

      if (!compromise.lossless()) {
        // The compromise diagnostic quotes the MEASURED numbers, not the
        // request. That is what makes FitCompromise a contract rather than a
        // promise: a build that stopped warning would also have to stop being
        // able to answer max_deviation_m.
        diag.say(Severity::Warning,
                 rules::kOsmFitApproximated,
                 segment_location(way.id, segment),
                 fmt::format("simplified {} node(s) to {} at {:.2f} m ({} merged as coincident); "
                             "greatest deviation from the source line {:.3f} m{}",
                             compromise.source_nodes,
                             compromise.kept_nodes,
                             compromise.tolerance_used_m,
                             compromise.merged_nodes,
                             compromise.max_deviation_m,
                             compromise.split_at_hairpin ? "; contains a hairpin turn" : ""));
      }

      const std::size_t index = plan.roads.size();
      joints[NodeKey{.node = road.start_node, .layer = road.layer}].emplace_back(
          index, ContactPoint::Start);
      joints[NodeKey{.node = road.end_node, .layer = road.layer}].emplace_back(index,
                                                                               ContactPoint::End);
      plan.roads.push_back(std::move(road));
    }
  }

  // --- 5. joints -----------------------------------------------------------
  for (auto& [key, ends] : joints) {
    if (ends.size() < 2) {
      continue; // a free end
    }
    if (ends.size() > options.max_junction_arms) {
      diag.say(Severity::Warning,
               rules::kOsmTopologyUnlinked,
               fmt::format("node/{}", key.node),
               fmt::format("{} road ends meet here, over this build's {}-arm limit; they are left "
                           "unjoined (junction generation is quadratic in arm count)",
                           ends.size(),
                           options.max_junction_arms));
      continue;
    }
    if (plan.joints.size() >= kMaxJunctions) {
      continue;
    }
    plan.joints.push_back(
        PlannedJoint{.node = key.node,
                     .layer = key.layer,
                     .kind = ends.size() == 2 ? JointKind::Link : JointKind::Junction,
                     .ends = ends});
  }

  // --- 5b. pull junction arms back --------------------------------------
  // Only JUNCTION joints: a degree-2 joint is a plain link and its two ends
  // must stay coincident, or check_linkable refuses and the roads never join.
  std::size_t trimmed = 0;
  std::size_t untrimmable = 0;
  for (const PlannedJoint& joint : plan.joints) {
    if (joint.kind != JointKind::Junction) {
      continue;
    }
    for (const auto& [index, contact] : joint.ends) {
      if (index >= plan.roads.size()) {
        continue;
      }
      if (trim_end(
              plan.roads[index].waypoints, contact == ContactPoint::Start, kJunctionArmSetbackM)) {
        ++trimmed;
      } else {
        ++untrimmable;
      }
    }
  }
  if (trimmed > 0) {
    diag.say(Severity::Info,
             rules::kOsmFitApproximated,
             "osm",
             fmt::format("{} junction arm(s) pulled back {:.0f} m so the junction generator has "
                         "room to build connecting roads; OSM states a crossing by sharing a "
                         "node, and arms meeting at a point leave none",
                         trimmed,
                         kJunctionArmSetbackM));
  }
  if (untrimmable > 0) {
    diag.say(Severity::Warning,
             rules::kOsmTopologyUnlinked,
             "osm",
             fmt::format("{} junction arm(s) were too short to pull back; their turns are likely "
                         "to be dropped by the junction generator",
                         untrimmable));
  }

  // A node whose ways sit on DIFFERENT layers appears under two keys and is
  // therefore never joined across them. Say so, or the absence looks like a
  // bug rather than the overpass rule working.
  std::map<OsmId, std::vector<int>> layers_at;
  for (const auto& [key, ends] : joints) {
    layers_at[key.node].push_back(key.layer);
  }
  for (const auto& [node, layers] : layers_at) {
    if (layers.size() > 1) {
      diag.say(Severity::Warning,
               rules::kOsmTopologyUnlinked,
               fmt::format("node/{}", node),
               fmt::format("road ends meet here on {} different layers and are NOT joined: this "
                           "is an overpass, not an intersection (see #496)",
                           layers.size()));
    }
  }

  // --- 6. bounds -----------------------------------------------------------
  if (!plan.roads.empty()) {
    double lo_x = std::numeric_limits<double>::max();
    double lo_y = std::numeric_limits<double>::max();
    double hi_x = std::numeric_limits<double>::lowest();
    double hi_y = std::numeric_limits<double>::lowest();
    for (const PlannedRoad& road : plan.roads) {
      for (const Waypoint& point : road.waypoints) {
        lo_x = std::min(lo_x, point.x);
        lo_y = std::min(lo_y, point.y);
        hi_x = std::max(hi_x, point.x);
        hi_y = std::max(hi_y, point.y);
      }
    }
    plan.bounds = {lo_x, lo_y, hi_x, hi_y};
  }

  if (graph.relation_count > 0) {
    diag.say(Severity::Info,
             rules::kOsmElementDropped,
             "osm",
             "OpenStreetMap data is ODbL-licensed; attribution is required for any work derived "
             "from it and is not yet written into the exported file (see #497)");
  }

  diag.flush();
  return result;
}

} // namespace roadmaker::osm
