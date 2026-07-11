#include "roadmaker/road/authoring.hpp"

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/tol.hpp"

// Implementation detail — Clothoids headers never leak into public headers.
#include <Clothoids.hh>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace roadmaker {

LaneProfile LaneProfile::two_lane_default() {
  return LaneProfile{
      .left = {LaneSpec{.type = LaneType::Driving, .width = 3.5, .outer_marking = true}},
      .right = {LaneSpec{.type = LaneType::Driving, .width = 3.5, .outer_marking = true},
                LaneSpec{.type = LaneType::Shoulder, .width = 1.0}},
      .center_marking = true,
  };
}

namespace {

/// A clothoid segment degrades to an arc or line when its curvature (rate)
/// is numerically zero — emit the simplest primitive so written OpenDRIVE
/// stays clean.
GeometryRecord to_record(const G2lib::ClothoidCurve& segment) {
  GeometryRecord record{
      .x = segment.x_begin(),
      .y = segment.y_begin(),
      .hdg = segment.theta_begin(),
      .length = segment.length(),
  };
  const double k0 = segment.kappa_begin();
  const double k1 = segment.kappa_end();
  const bool constant_curvature = std::abs(k1 - k0) < tol::kCurvatureEpsilon;
  if (constant_curvature && std::abs(k0) < tol::kCurvatureEpsilon) {
    record.shape = LineGeom{};
  } else if (constant_curvature) {
    record.shape = ArcGeom{.curvature = k0};
  } else {
    record.shape = SpiralGeom{.curv_start = k0, .curv_end = k1};
  }
  return record;
}

/// Shared front half of both fit overloads: validation + coordinate split.
Expected<std::pair<std::vector<double>, std::vector<double>>>
waypoint_coordinates(std::span<const Waypoint> waypoints) {
  if (waypoints.size() < 2) {
    return make_error(ErrorCode::InvalidArgument, "need at least 2 waypoints");
  }
  for (std::size_t i = 1; i < waypoints.size(); ++i) {
    const double dx = waypoints[i].x - waypoints[i - 1].x;
    const double dy = waypoints[i].y - waypoints[i - 1].y;
    if (std::hypot(dx, dy) < tol::kLength) {
      return make_error(
          ErrorCode::InvalidArgument, "coincident consecutive waypoints", std::to_string(i));
    }
  }
  std::vector<double> xs;
  std::vector<double> ys;
  xs.reserve(waypoints.size());
  ys.reserve(waypoints.size());
  for (const Waypoint& p : waypoints) {
    xs.push_back(p.x);
    ys.push_back(p.y);
  }
  return std::pair{std::move(xs), std::move(ys)};
}

ReferenceLine to_reference_line(const G2lib::ClothoidList& path) {
  ReferenceLine line;
  for (int i = 0; i < path.num_segments(); ++i) {
    line.append(to_record(path.get(i)));
  }
  return line;
}

} // namespace

Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints) {
  auto coordinates = waypoint_coordinates(waypoints);
  if (!coordinates.has_value()) {
    return tl::unexpected<Error>(coordinates.error());
  }
  const auto& [xs, ys] = *coordinates;

  // G1 clothoid spline through the waypoints; angles are estimated by the
  // library (never hand-roll Fresnel math).
  G2lib::ClothoidList path("rm_author");
  if (!path.build_G1(static_cast<int>(waypoints.size()), xs.data(), ys.data())) {
    return make_error(ErrorCode::InvalidArgument, "clothoid G1 fit failed");
  }
  return to_reference_line(path);
}

Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints,
                                          std::span<const double> headings) {
  if (headings.size() != waypoints.size()) {
    return make_error(ErrorCode::InvalidArgument, "one heading per waypoint required");
  }
  auto coordinates = waypoint_coordinates(waypoints);
  if (!coordinates.has_value()) {
    return tl::unexpected<Error>(coordinates.error());
  }
  const auto& [xs, ys] = *coordinates;

  G2lib::ClothoidList path("rm_author");
  if (!path.build_G1(static_cast<int>(waypoints.size()), xs.data(), ys.data(), headings.data())) {
    return make_error(ErrorCode::InvalidArgument, "clothoid G1 Hermite fit failed");
  }
  return to_reference_line(path);
}

Expected<RoadId> author_clothoid_road(RoadNetwork& network,
                                      std::span<const Waypoint> waypoints,
                                      const LaneProfile& profile,
                                      std::string name,
                                      std::string odr_id) {
  auto line = fit_clothoid_path(waypoints);
  if (!line.has_value()) {
    return tl::unexpected<Error>(line.error());
  }
  if (profile.left.empty() && profile.right.empty()) {
    return make_error(ErrorCode::InvalidArgument, "lane profile has no lanes");
  }
  for (const auto& side : {profile.left, profile.right}) {
    for (const LaneSpec& spec : side) {
      if (spec.width <= 0.0) {
        return make_error(ErrorCode::InvalidArgument, "lane width must be > 0");
      }
    }
  }

  // Auto-assign the next free numeric odr id when none was given.
  if (odr_id.empty()) {
    int candidate = 1;
    while (network.find_road(std::to_string(candidate)).is_valid()) {
      ++candidate;
    }
    odr_id = std::to_string(candidate);
  } else if (network.find_road(odr_id).is_valid()) {
    return make_error(ErrorCode::InvalidArgument, "road odr_id already in use", odr_id);
  }

  const RoadId road_id = network.create_road(std::move(name), std::move(odr_id));
  Road& road = *network.road(road_id);
  road.plan_view = std::move(*line);
  road.length = road.plan_view.length();
  road.authoring_waypoints.emplace(waypoints.begin(), waypoints.end());

  const LaneSectionId section = network.add_lane_section(road_id, 0.0);
  const LaneId center = network.add_lane(section, 0, LaneType::None);
  if (profile.center_marking) {
    network.lane(center)->road_marks.push_back(
        RoadMark{.s_offset = 0.0, .type = RoadMarkType::Broken});
  }
  for (std::size_t i = 0; i < profile.left.size(); ++i) {
    const LaneSpec& spec = profile.left[i];
    const LaneId lane = network.add_lane(section, static_cast<int>(i) + 1, spec.type);
    network.lane(lane)->widths.push_back(Poly3{.a = spec.width});
    if (spec.outer_marking) {
      network.lane(lane)->road_marks.push_back(
          RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid});
    }
  }
  for (std::size_t i = 0; i < profile.right.size(); ++i) {
    const LaneSpec& spec = profile.right[i];
    const LaneId lane = network.add_lane(section, -(static_cast<int>(i) + 1), spec.type);
    network.lane(lane)->widths.push_back(Poly3{.a = spec.width});
    if (spec.outer_marking) {
      network.lane(lane)->road_marks.push_back(
          RoadMark{.s_offset = 0.0, .type = RoadMarkType::Solid});
    }
  }
  return road_id;
}

} // namespace roadmaker
