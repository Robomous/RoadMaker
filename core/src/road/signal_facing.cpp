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

#include "roadmaker/road/signal_facing.hpp"

#include "roadmaker/road/defaults.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <fmt/format.h>

#include <cmath>
#include <limits>
#include <optional>

namespace roadmaker {

namespace {

/// Right-hand traffic: a lane RIGHT of the reference line (negative id) runs
/// toward +s, a lane LEFT of it runs toward -s. `@direction=reversed` flips
/// that; `both` keeps the grouping's answer, since a bidirectional lane gives
/// a sign no reason to prefer either approach.
///
/// RoadMaker does not model <road @rule>, so RHT is the standing assumption
/// here as it is in the junction-approach helpers (see edit::driving_lanes_at
/// and mesh::approach_orientation, which encode the same convention).
TravelDirection lane_travel(int odr_id, LaneDirection direction) {
  const bool toward_plus_s = odr_id < 0;
  const bool flipped = direction == LaneDirection::Reversed;
  return (toward_plus_s != flipped) ? TravelDirection::Forward : TravelDirection::Backward;
}

/// Distance from `t` to the closed interval [lo, hi]; zero when inside.
double distance_to_span(double t, double lo, double hi) {
  if (t < lo) {
    return lo - t;
  }
  if (t > hi) {
    return t - hi;
  }
  return 0.0;
}

/// The connecting road's own junction connection, if `road` is one. Returns
/// nullptr for an ordinary road (including an APPROACH to a junction — an
/// approach's lanes are the real carriageway and are read normally).
const JunctionConnection*
connection_for_connecting_road(const RoadNetwork& network, const Road& road, RoadId road_id) {
  if (!road.junction.is_valid()) {
    return nullptr;
  }
  const Junction* junction = network.junction(road.junction);
  if (junction == nullptr) {
    return nullptr;
  }
  for (const JunctionConnection& connection : junction->connections) {
    if (connection.connecting_road == road_id) {
      return &connection;
    }
  }
  return nullptr;
}

} // namespace

Expected<SignalApproach>
signal_approach(const RoadNetwork& network, RoadId road_id, double s, double t) {
  const Road* road = network.road(road_id);
  if (road == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "signal facing: stale road id");
  }
  if (road->plan_view.empty()) {
    return make_error(ErrorCode::InvalidArgument, "signal facing: road has no plan view");
  }
  const double length = road->plan_view.length();
  if (s < -tol::kLength || s > length + tol::kLength) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("signal facing: s {} outside the road's [0, {}]", s, length));
  }

  // A connecting road inside a junction takes its direction from the approach
  // that feeds it, never from its own lanes. Traffic enters at the contact
  // point the connection names and travels AWAY from it.
  if (const JunctionConnection* connection =
          connection_for_connecting_road(network, *road, road_id);
      connection != nullptr) {
    return SignalApproach{.travel = connection->contact_point == ContactPoint::Start
                                        ? TravelDirection::Forward
                                        : TravelDirection::Backward,
                          .side = t > 0.0 ? 1.0 : -1.0,
                          .from_junction_approach = true,
                          .has_driving_lane = true};
  }

  // Nearest driving lane to t: the Driving lane whose [inner, outer] span is
  // closest, ties going to the one on t's own side. lane_boundary_offsets is
  // the single routine that turns a cross section into lane edges, so the
  // spans here are exactly the ones the mesher draws and the picker hits.
  //
  // Its vector runs LEFTMOST EDGE FIRST and covers only the width-bearing
  // lanes: the centre lane 0 is a boundary, not a span, so the count is
  // (lanes - 1) + 1. Walking the two in step therefore means advancing the
  // edge cursor for every lane EXCEPT lane 0.
  const LaneSectionId section_id = section_at(network, road_id, s);
  const LaneSection* section = network.lane_section(section_id);
  const std::vector<double> boundaries = lane_boundary_offsets(network, road_id, s);

  std::optional<TravelDirection> best_travel;
  std::optional<double> best_side;
  double best_distance = std::numeric_limits<double>::max();
  bool best_same_side = false;

  if (section != nullptr && boundaries.size() >= 2) {
    const double placed_side = t > 0.0 ? 1.0 : -1.0;
    std::size_t edge = 0;
    for (const LaneId lane_id : section->lanes) {
      const Lane* lane = network.lane(lane_id);
      if (lane == nullptr) {
        break; // a stale lane id desyncs the edge cursor: stop rather than skip
      }
      if (lane->odr_id == 0) {
        continue; // lane 0 owns no span, so it does not consume an edge
      }
      if (edge + 1 >= boundaries.size()) {
        break; // cross section and edge list disagree: stop rather than guess
      }
      const double outer = boundaries[edge];
      const double inner = boundaries[edge + 1];
      ++edge;
      if (lane->type != LaneType::Driving) {
        continue;
      }
      const double lo = std::min(inner, outer);
      const double hi = std::max(inner, outer);
      const double distance = distance_to_span(t, lo, hi);
      const double lane_side = lane->odr_id > 0 ? 1.0 : -1.0;
      const bool same_side = lane_side == placed_side;

      // Closest wins. A tie goes to the lane on the side the sign was placed:
      // a plate on the right shoulder is governed by the carriageway it stands
      // beside, not by the one across the centre line.
      const bool closer = distance < best_distance - tol::kLength;
      const bool tied_and_better_side =
          std::abs(distance - best_distance) <= tol::kLength && same_side && !best_same_side;
      if (!best_travel.has_value() || closer || tied_and_better_side) {
        best_distance = distance;
        best_travel = lane_travel(lane->odr_id, lane->direction);
        best_side = lane_side;
        best_same_side = same_side;
      }
    }
  }

  if (!best_travel.has_value()) {
    // No driving lane in this cross section (a sidewalk-only stretch, or a
    // road still being authored). The side convention alone still yields the
    // facing a reader expects: a sign on the right governs +s traffic.
    return SignalApproach{.travel = t > 0.0 ? TravelDirection::Backward : TravelDirection::Forward,
                          .side = t > 0.0 ? 1.0 : -1.0,
                          .from_junction_approach = false,
                          .has_driving_lane = false};
  }

  // t == 0 sits ON the reference line and has no side of its own; the
  // governing lane's side stands in for it.
  const double side = std::abs(t) <= tol::kLength ? *best_side : (t > 0.0 ? 1.0 : -1.0);
  return SignalApproach{.travel = *best_travel,
                        .side = side,
                        .from_junction_approach = false,
                        .has_driving_lane = true};
}

Expected<SignalFacing>
auto_signal_facing(const RoadNetwork& network, RoadId road_id, double s, double t) {
  const Expected<SignalApproach> approach = signal_approach(network, road_id, s, t);
  if (!approach.has_value()) {
    return tl::unexpected<Error>(approach.error());
  }

  const double travel = approach->travel == TravelDirection::Forward ? 1.0 : -1.0;

  // §14.1: "+" applies to traffic travelling toward +s, and a signal so marked
  // with hOffset 0 FACES that traffic. So the orientation the face must carry
  // is simply the direction its traffic runs.
  const ObjectOrientation orientation =
      travel > 0.0 ? ObjectOrientation::Plus : ObjectOrientation::Minus;

  // Toe-out: cant the face AWAY from the roadway, toward the sign's own side.
  //
  // The datum for Plus is road_hdg + pi and for Minus is road_hdg; +t lies at
  // road_hdg + pi/2 and -t at road_hdg - pi/2. Working the four combinations
  // through, a counter-clockwise (positive) offset turns the face outward
  // exactly when side and travel DISAGREE in sign:
  //
  //   right (-1) + forward  (+1) -> +toe   left (+1) + backward (-1) -> +toe
  //   right (-1) + backward (-1) -> -toe   left (+1) + forward  (+1) -> -toe
  const double h_offset = -(approach->side * travel) * defaults::kSignToeOut;

  return SignalFacing{.orientation = orientation, .h_offset = h_offset};
}

} // namespace roadmaker
