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

#include "document/actor_placement.hpp"

#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <set>
#include <string_view>
#include <vector>

#include "document/prop_placement.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

/// One driving lane at a station: its OpenDRIVE id and the t-interval it
/// spans, with the t of its centre line.
struct LaneSpan {
  int odr_id = 0;
  double lo = 0.0;
  double hi = 0.0;
  double centre = 0.0;
};

/// The driving lanes of `road` at station `s`, from the kernel's own boundary
/// walk.
///
/// `lane_boundary_offsets` returns N+1 boundaries left-to-right for N lanes,
/// so boundary[i] and boundary[i+1] bracket the i-th lane — and the lane order
/// matches `section.lanes`, which the arena keeps sorted by OpenDRIVE id
/// DESCENDING (leftmost first). Reusing the kernel walk rather than
/// re-accumulating widths here is what keeps a snapped actor in the same place
/// the mesher draws the lane.
std::vector<LaneSpan> driving_lane_spans(const RoadNetwork& network, RoadId road_id, double s) {
  std::vector<LaneSpan> spans;

  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return spans;
  }
  const LaneSection* section = network.lane_section(section_at(network, road_id, s));
  if (section == nullptr) {
    return spans;
  }
  const std::vector<double> boundaries = lane_boundary_offsets(network, road_id, s);
  if (boundaries.size() < 2) {
    return spans;
  }

  // section.lanes includes lane 0, which lane_boundary_offsets does NOT give a
  // span of — it is a boundary, not a lane. Walking the two in lockstep with an
  // index that skips lane 0 is what keeps them aligned.
  std::size_t boundary = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane == nullptr) {
      continue;
    }
    if (lane->odr_id == 0) {
      continue; // the centre lane has no width and carries no traffic
    }
    if (boundary + 1 >= boundaries.size()) {
      break;
    }
    const double a = boundaries[boundary];
    const double b = boundaries[boundary + 1];
    ++boundary;

    if (lane->type != LaneType::Driving) {
      continue; // a sidewalk, a median, a shoulder — not somewhere an actor goes
    }
    const double lo = std::min(a, b);
    const double hi = std::max(a, b);
    if (hi - lo <= 0.0) {
      continue; // a zero-width lane is legal and cannot hold anything
    }
    spans.push_back(LaneSpan{.odr_id = lane->odr_id, .lo = lo, .hi = hi, .centre = (a + b) / 2.0});
  }
  return spans;
}

/// How far the reconstruction of a station may miss the cursor before the drop
/// counts as OFF THE END of the road [m]. Far below a lane width, far above the
/// station solver's convergence residual.
constexpr double kLongitudinalSlack = 0.05;

/// True when (x, y) is genuinely beside `road`, rather than off one of its ends.
///
/// ★ THE TRAP THIS EXISTS FOR, and it is documented in picking.hpp itself:
/// "find_station bounds s to the line's length but leaves t unbounded, so it
/// reports a confident station for a point out in open space". A cursor 300 m
/// PAST the end of a straight road projects to s = length with t = 0 — a
/// perfectly plausible-looking station with a tiny |t|, which every lateral
/// threshold accepts.
///
/// So the longitudinal overshoot has to be measured separately, and this is the
/// exact way to do it: `station_to_world` reconstructs the point from (s, t)
/// and therefore recovers only the LATERAL part, discarding any longitudinal
/// residual. The distance between the reconstruction and the cursor IS the
/// overshoot.
bool station_reconstructs_the_cursor(const Road& road,
                                     const StationCoord& station,
                                     double x,
                                     double y) {
  const std::array<double, 2> back = station_to_world(road.plan_view, station.s, station.t);
  const double dx = back[0] - x;
  const double dy = back[1] - y;
  return std::hypot(dx, dy) <= kLongitudinalSlack;
}

/// The span containing `t`, or the nearest one when `t` falls between lanes
/// (a median gap) — nullptr when the road has no driving lane at all.
const LaneSpan* span_for_t(const std::vector<LaneSpan>& spans, double t) {
  const LaneSpan* nearest = nullptr;
  double best = 0.0;
  for (const LaneSpan& span : spans) {
    if (t >= span.lo && t <= span.hi) {
      return &span;
    }
    const double distance = t < span.lo ? span.lo - t : t - span.hi;
    if (nearest == nullptr || distance < best) {
      nearest = &span;
      best = distance;
    }
  }
  return nearest;
}

} // namespace

std::optional<LaneAnchor>
nearest_lane_anchor(const RoadNetwork& network, double x, double y, double max_t) {
  // The SAME snap props use — one funnel, so "over a road" means the same thing
  // to both tools.
  const std::optional<RoadStation> station = nearest_road_station(network, x, y, max_t);
  if (!station.has_value()) {
    return std::nullopt;
  }
  const Road* road = network.road(station->road);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  if (!station_reconstructs_the_cursor(
          *road, StationCoord{.s = station->s, .t = station->t}, x, y)) {
    return std::nullopt; // off one of the ends — see station_reconstructs_the_cursor
  }

  const std::vector<LaneSpan> spans = driving_lane_spans(network, station->road, station->s);
  const LaneSpan* lane = span_for_t(spans, station->t);
  if (lane == nullptr) {
    return std::nullopt;
  }

  return LaneAnchor{
      .road = station->road,
      .road_odr_id = road->odr_id,
      .lane_odr_id = std::to_string(lane->odr_id),
      .s = station->s,
      .offset = 0.0, // the lane centre, which is where an actor belongs
  };
}

std::string actor_drop_hint(const RoadNetwork& network, double x, double y, double max_t) {
  const std::optional<RoadStation> station = nearest_road_station(network, x, y, max_t);
  if (!station.has_value()) {
    return "No road within reach — an actor is placed on a lane, not in space.";
  }
  const Road* road = network.road(station->road);
  if (road == nullptr || road->plan_view.empty()) {
    return "No road within reach — an actor is placed on a lane, not in space.";
  }
  if (!station_reconstructs_the_cursor(
          *road, StationCoord{.s = station->s, .t = station->t}, x, y)) {
    return "Past the end of the road — move the cursor onto the carriageway.";
  }
  if (driving_lane_spans(network, station->road, station->s).empty()) {
    // ★ NOT "no road in reach". There IS a road; it has no driving lane here,
    // which is a different problem with a different fix, and saying the wrong
    // one sends the user looking in the wrong place.
    return "That road has no driving lane here — an actor cannot stand on a "
           "sidewalk or a median.";
  }
  return {};
}

std::string next_actor_name(const osc::Scenario& scenario, std::string_view key) {
  std::set<std::string> taken;
  for (const osc::ScenarioObject& object : scenario.entities.scenario_objects) {
    taken.insert(object.name);
  }

  // "car" -> "Car1". The stem is capitalized so the name reads as a proper
  // noun in the scene tree and in every entityRef.
  std::string stem(key);
  if (!stem.empty()) {
    stem[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stem[0])));
  }

  for (int index = 1;; ++index) {
    std::string candidate = fmt::format("{}{}", stem, index);
    if (taken.count(candidate) == 0) {
      return candidate;
    }
  }
}

std::optional<ActorPose> actor_world_pose(const RoadNetwork& network,
                                          const osc::LanePosition& position) {
  // The .xosc carries OpenDRIVE id STRINGS, so this is where they come back to
  // arena handles — and where a reference to a road that no longer exists shows
  // up as an actor that cannot be drawn rather than as one drawn at the origin.
  const Road* found = nullptr;
  RoadId road_id;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (found == nullptr && road.odr_id == position.road_id) {
      found = &road;
      road_id = id;
    }
  });
  if (found == nullptr || found->plan_view.empty()) {
    return std::nullopt;
  }

  int lane_odr_id = 0;
  try {
    lane_odr_id = std::stoi(position.lane_id);
  } catch (...) {
    // A lane id that is not an integer is legal in OpenSCENARIO (the temporary
    // lane layer) and simply not resolvable here.
    return std::nullopt;
  }

  const double s = std::clamp(position.s, 0.0, found->length);
  const std::vector<LaneSpan> spans = driving_lane_spans(network, road_id, s);
  const auto span = std::ranges::find_if(
      spans, [lane_odr_id](const LaneSpan& candidate) { return candidate.odr_id == lane_odr_id; });
  if (span == spans.end()) {
    return std::nullopt;
  }

  const double t = span->centre + position.offset;
  const std::array<double, 2> world = station_to_world(found->plan_view, s, t);
  const PathPoint point = found->plan_view.evaluate(s);

  // ★ TRAVEL DIRECTION, NOT THE REFERENCE LINE. A right-hand lane (negative id)
  // travels with +s; a left-hand lane travels against it. An actor facing
  // backwards down its lane renders convincingly and simulates absurdly, and
  // nothing downstream would flag it.
  const double heading = lane_odr_id < 0 ? point.hdg : point.hdg + std::numbers::pi;

  return ActorPose{.position = {world[0], world[1], 0.0}, .heading = heading};
}

std::optional<ActorPose> actor_world_pose(const RoadNetwork& network, const LaneAnchor& anchor) {
  return actor_world_pose(network, to_lane_position(anchor));
}

osc::LanePosition to_lane_position(const LaneAnchor& anchor) {
  osc::LanePosition position;
  position.road_id = anchor.road_odr_id;
  position.lane_id = anchor.lane_odr_id;
  position.s = anchor.s;
  position.offset = anchor.offset;
  return position;
}

} // namespace roadmaker::editor
