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

#include "document/route_preview.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/lane_section.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <algorithm>
#include <variant>

#include "viewport/picking.hpp"

namespace roadmaker::editor {
namespace {

/// The centre-line t of the lane with OpenDRIVE id `lane_odr_id` at station
/// `s`, from the kernel's own boundary walk — the same walk
/// `driving_lane_spans` uses for the actor snap, but WITHOUT the driving-only
/// filter: `resolve_route` never restricted the leg to a driving lane, and a
/// preview that refused to draw what the resolver solved would look like a
/// resolution failure.
std::optional<double>
lane_centre_t(const RoadNetwork& network, RoadId road_id, int lane_odr_id, double s) {
  const LaneSection* section = network.lane_section(section_at(network, road_id, s));
  if (section == nullptr) {
    return std::nullopt;
  }
  const std::vector<double> boundaries = lane_boundary_offsets(network, road_id, s);
  if (boundaries.size() < 2) {
    return std::nullopt;
  }
  // boundary[i] and boundary[i+1] bracket the i-th non-centre lane;
  // section.lanes is sorted by OpenDRIVE id descending (leftmost first), and
  // lane 0 is a boundary rather than a lane, so it advances no index.
  std::size_t boundary = 0;
  for (const LaneId lane_id : section->lanes) {
    const Lane* lane = network.lane(lane_id);
    if (lane == nullptr) {
      continue;
    }
    if (lane->odr_id == 0) {
      continue;
    }
    if (boundary + 1 >= boundaries.size()) {
      break;
    }
    const double a = boundaries[boundary];
    const double b = boundaries[boundary + 1];
    ++boundary;
    if (lane->odr_id == lane_odr_id) {
      return (a + b) / 2.0;
    }
  }
  return std::nullopt;
}

} // namespace

std::vector<std::array<double, 3>> route_leg_polyline(const RoadNetwork& network,
                                                      const osc::RouteLeg& leg) {
  std::vector<std::array<double, 3>> points;

  const Road* road = network.road(leg.road);
  if (road == nullptr || road->plan_view.empty()) {
    return points;
  }

  const double lo = std::min(leg.s_start, leg.s_end);
  const double hi = std::max(leg.s_start, leg.s_end);

  // The kernel's curvature-adaptive stations, clipped to the leg and with the
  // leg's own endpoints forced in — so a short leg still gets its exact ends
  // and a curved one gets enough fill to not cut the corner.
  std::vector<double> stations =
      sample_stations(road->plan_view, SamplingOptions{.extra_stations = {lo, hi}});
  std::erase_if(stations, [lo, hi](double s) { return s < lo || s > hi; });
  if (stations.empty()) {
    return points;
  }

  for (const double s : stations) {
    const std::optional<double> centre = lane_centre_t(network, leg.road, leg.lane_odr_id, s);
    if (!centre.has_value()) {
      // The lane is gone at this station (a re-section, a deletion). Stop
      // rather than jump lanes: a polyline that bridged the gap would draw a
      // path the resolver never solved.
      break;
    }
    const std::array<double, 2> world = station_to_world(road->plan_view, s, *centre);
    points.push_back({world[0], world[1], 0.0});
  }

  // `sample_stations` is ascending; a leg travelling against +s (a left-hand
  // lane) runs high-to-low, and the caller is promised travel order.
  if (leg.s_end < leg.s_start) {
    std::ranges::reverse(points);
  }
  return points;
}

std::vector<std::optional<ActorPose>> route_waypoint_poses(const RoadNetwork& network,
                                                           const osc::Route& route) {
  std::vector<std::optional<ActorPose>> poses;
  poses.reserve(route.waypoints.size());
  for (const osc::RouteWaypoint& waypoint : route.waypoints) {
    if (const auto* lane = std::get_if<osc::LanePosition>(&waypoint.position)) {
      poses.push_back(actor_world_pose(network, *lane));
    } else {
      poses.push_back(std::nullopt);
    }
  }
  return poses;
}

} // namespace roadmaker::editor
