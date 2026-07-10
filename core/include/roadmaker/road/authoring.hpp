#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"

#include <span>
#include <string>
#include <vector>

namespace roadmaker {

/// Plan-view waypoint [m] for clothoid path fitting.
struct Waypoint {
  double x = 0.0;
  double y = 0.0;
};

/// One lane of an authored road's constant-width cross section.
struct LaneSpec {
  LaneType type = LaneType::Driving;

  /// Constant width [m]; must be > 0.
  double width = 3.5;

  /// Paint a solid marking on this lane's outer boundary.
  bool outer_marking = false;
};

/// Cross-section blueprint applied to the whole authored road.
struct LaneProfile {
  /// Lanes left of the reference line, innermost (+1) first.
  std::vector<LaneSpec> left;

  /// Lanes right of the reference line, innermost (-1) first.
  std::vector<LaneSpec> right;

  /// Paint a broken center-line marking on lane 0.
  bool center_marking = true;

  /// Two-lane rural default: one driving lane each way, edge lines.
  [[nodiscard]] RM_API static LaneProfile two_lane_default();
};

/// Fits a G1 clothoid path through `waypoints` (headings estimated by the
/// Clothoids G1 spline), creates a road with `profile`, and inserts it into
/// the network. Consecutive joints are G1: position and heading continuous
/// within rm::tol.
///
/// Errors (InvalidArgument): fewer than 2 waypoints, coincident consecutive
/// waypoints, an odr_id already in use, or an empty lane profile.
[[nodiscard]] RM_API Expected<RoadId> author_clothoid_road(RoadNetwork& network,
                                                           std::span<const Waypoint> waypoints,
                                                           const LaneProfile& profile,
                                                           std::string name = {},
                                                           std::string odr_id = {});

} // namespace roadmaker
