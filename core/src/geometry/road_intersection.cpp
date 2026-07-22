// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "roadmaker/geometry/road_intersection.hpp"

#include "roadmaker/road/network.hpp"
#include "roadmaker/tol.hpp"

// Clothoids is an implementation detail — it must never leak into a header.
#include <Clothoids.hh>

#include <algorithm>
#include <cmath>
#include <optional>
#include <variant>
#include <vector>

namespace roadmaker {

namespace {

/// Builds a Clothoids list matching `line` record-for-record. line/arc/spiral
/// records map exactly onto a clothoid (k, dk, L); a paramPoly3 record is
/// approximated by the single G1 clothoid through its endpoints (a rare
/// imported shape — the authoring tools never emit paramPoly3). Throws
/// Utils::Runtime_Error from Clothoids on a degenerate record — callers wrap
/// this in the kernel↔Clothoids exception boundary.
G2lib::ClothoidList to_clothoid_list(const ReferenceLine& line) {
  G2lib::ClothoidList list("rm_intersect");
  const auto& records = line.records();
  for (const GeometryRecord& record : records) {
    G2lib::ClothoidCurve curve("rm_intersect_seg");
    if (const auto* arc = std::get_if<ArcGeom>(&record.shape)) {
      curve.build(record.x, record.y, record.hdg, arc->curvature, 0.0, record.length);
    } else if (const auto* spiral = std::get_if<SpiralGeom>(&record.shape)) {
      const double dk = (spiral->curv_end - spiral->curv_start) / record.length;
      curve.build(record.x, record.y, record.hdg, spiral->curv_start, dk, record.length);
    } else if (std::holds_alternative<LineGeom>(record.shape)) {
      curve.build(record.x, record.y, record.hdg, 0.0, 0.0, record.length);
    } else {
      // paramPoly3: G1 clothoid through the record's endpoint poses.
      const PathPoint start = line.evaluate(record.s);
      const PathPoint end = line.evaluate(record.s + record.length);
      curve.build_G1(start.x, start.y, start.hdg, end.x, end.y, end.hdg);
    }
    list.push_back(curve);
  }
  return list;
}

/// Crossings of two reference lines as (station on a, station on b) pairs,
/// ascending by station on a. Empty when either line is empty or they miss.
std::optional<std::vector<RoadCrossing>> intersect_lines(const ReferenceLine& a,
                                                         const ReferenceLine& b) {
  if (a.empty() || b.empty()) {
    return std::vector<RoadCrossing>{};
  }
  try {
    const G2lib::ClothoidList list_a = to_clothoid_list(a);
    const G2lib::ClothoidList list_b = to_clothoid_list(b);
    G2lib::IntersectList hits;
    list_a.intersect(list_b, hits);
    std::vector<RoadCrossing> crossings;
    crossings.reserve(hits.size());
    for (const auto& [s_a, s_b] : hits) {
      const PathPoint point = a.evaluate(s_a);
      crossings.push_back(
          RoadCrossing{.s_a = s_a, .s_b = s_b, .point = Waypoint{.x = point.x, .y = point.y}});
    }
    std::ranges::sort(crossings, {}, &RoadCrossing::s_a);
    return crossings;
  } catch (...) {
    return std::nullopt;
  }
}

/// Strictly inside (0, length): an endpoint touch is a tee/link, not a cross.
bool interior(double s, double length) {
  return s > tol::kLength && s < length - tol::kLength;
}

} // namespace

Expected<std::vector<RoadCrossing>>
road_intersections(const RoadNetwork& network, RoadId a, RoadId b) {
  const Road* road_a = network.road(a);
  const Road* road_b = network.road(b);
  if (road_a == nullptr || road_b == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "stale road id");
  }
  if (a == b) {
    return make_error(ErrorCode::InvalidArgument, "a road cannot cross itself");
  }
  if (road_a->plan_view.empty() || road_b->plan_view.empty()) {
    return make_error(ErrorCode::InvalidArgument, "road has no plan-view geometry");
  }
  auto crossings = intersect_lines(road_a->plan_view, road_b->plan_view);
  if (!crossings.has_value()) {
    return make_error(ErrorCode::InvalidArgument, "clothoid intersection failed");
  }
  return std::move(*crossings);
}

std::optional<BodyCrossing>
first_body_crossing(const RoadNetwork& network, const ReferenceLine& fitted, RoadId exclude) {
  if (fitted.empty()) {
    return std::nullopt;
  }
  std::optional<BodyCrossing> best;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (id == exclude || road.plan_view.empty() || road.junction.is_valid()) {
      return; // skip the excluded road and generated connecting roads
    }
    const auto crossings = intersect_lines(fitted, road.plan_view);
    if (!crossings.has_value()) {
      return;
    }
    for (const RoadCrossing& crossing : *crossings) {
      if (!interior(crossing.s_a, fitted.length()) ||
          !interior(crossing.s_b, road.plan_view.length())) {
        continue;
      }
      if (!best.has_value() || crossing.s_a < best->s_line) {
        best = BodyCrossing{
            .road = id, .s_line = crossing.s_a, .s_road = crossing.s_b, .point = crossing.point};
      }
    }
  });
  return best;
}

} // namespace roadmaker
