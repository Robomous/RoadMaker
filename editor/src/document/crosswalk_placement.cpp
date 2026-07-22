// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "document/crosswalk_placement.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <cmath>
#include <numbers>

#include "viewport/picking.hpp"

namespace roadmaker::editor {

std::optional<std::pair<RoadId, Object>> crosswalk_for_arm(const RoadNetwork& network,
                                                           JunctionId junction,
                                                           RoadId arm_road,
                                                           const edit::CrosswalkParams& params) {
  for (auto& [road, object] : edit::junction_crosswalks(network, junction, params)) {
    if (road == arm_road) {
      return std::pair<RoadId, Object>{road, std::move(object)};
    }
  }
  return std::nullopt;
}

std::optional<ArmHit>
nearest_junction_arm(const RoadNetwork& network, double x, double y, double threshold) {
  std::optional<ArmHit> best;
  double best_dist = threshold;
  network.for_each_road([&](RoadId id, const Road& road) {
    // A connecting road inside the junction is never an approach; only an
    // incoming arm carries a crosswalk + stop line.
    if (road.plan_view.empty() || road.junction.is_valid()) {
      return;
    }
    // True distance to the nearest point on the (clamped) reference line, not
    // just the perpendicular t: a cursor colinear with the arm but beyond its
    // end has a near-zero t yet is far from the road, and must be rejected.
    const StationCoord station = find_station(road.plan_view, x, y);
    const auto foot = road.plan_view.evaluate(station.s);
    const double dist = std::hypot(x - foot.x, y - foot.y);
    if (dist > best_dist) {
      return;
    }
    // Which end (if any) attaches to a junction — the end nearer the cursor wins
    // when a road tees two junctions.
    const double length = road.plan_view.length();
    std::optional<ContactPoint> contact;
    double end_s = 0.0;
    for (const ContactPoint candidate : {ContactPoint::Start, ContactPoint::End}) {
      if (!edit::junction_at_end(network, RoadEnd{.road = id, .contact = candidate}).has_value()) {
        continue;
      }
      const double s_end = candidate == ContactPoint::Start ? 0.0 : length;
      if (!contact.has_value() || std::abs(station.s - s_end) < std::abs(station.s - end_s)) {
        contact = candidate;
        end_s = s_end;
      }
    }
    if (!contact.has_value()) {
      return; // this road touches no junction
    }
    const std::optional<JunctionId> junction =
        edit::junction_at_end(network, RoadEnd{.road = id, .contact = *contact});
    const auto anchor = station_to_world(road.plan_view, end_s, 0.0);
    // Heading INTO the junction: +s at the End end points into it; at the Start
    // end the reference tangent points away, so reverse it.
    const double s_hdg = road.plan_view.evaluate(end_s).hdg;
    const double into = *contact == ContactPoint::End ? s_hdg : s_hdg + std::numbers::pi;
    best_dist = dist;
    best = ArmHit{.junction = *junction,
                  .arm_road = id,
                  .contact = *contact,
                  .anchor_x = anchor[0],
                  .anchor_y = anchor[1],
                  .heading = into};
  });
  return best;
}

} // namespace roadmaker::editor
