#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/lane.hpp"
#include "roadmaker/road/network.hpp"

#include <span>
#include <string>
#include <vector>

namespace roadmaker {

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

/// Fits a G1 clothoid reference line through `waypoints` (headings
/// estimated by the Clothoids G1 spline). Consecutive joints are G1:
/// position and heading continuous within rm::tol. This is the same fit
/// the authoring API and the node-edit commands use.
///
/// Errors (InvalidArgument): fewer than 2 waypoints, coincident consecutive
/// waypoints, or a failed spline fit.
[[nodiscard]] RM_API Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints);

/// Fits a G1 clothoid path through `waypoints`, creates a road with
/// `profile`, and inserts it into the network. The waypoints are recorded
/// as Road::authoring_waypoints.
///
/// Errors (InvalidArgument): the fit_clothoid_path errors, an odr_id
/// already in use, or an empty lane profile.
[[nodiscard]] RM_API Expected<RoadId> author_clothoid_road(RoadNetwork& network,
                                                           std::span<const Waypoint> waypoints,
                                                           const LaneProfile& profile,
                                                           std::string name = {},
                                                           std::string odr_id = {});

} // namespace roadmaker
