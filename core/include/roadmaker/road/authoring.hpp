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

  // Cross-section templates the Create Road tool offers (02_editing_tools.md
  // §2). Contents are content-tested — changing a template is a visible
  // product change, not a refactor.

  /// Two-lane rural: one driving lane each way with solid edge lines and a
  /// right-hand shoulder, broken center line.
  [[nodiscard]] RM_API static LaneProfile two_lane_rural();

  /// Urban street: one driving lane each way with solid edge lines and a
  /// sidewalk on both sides, broken center line.
  [[nodiscard]] RM_API static LaneProfile urban_sidewalk();

  /// Highway: two driving lanes each way with a solid outer edge line and a
  /// wide shoulder on both sides; no center marking (directions are treated
  /// as separated — median modelling is out of M2 scope).
  [[nodiscard]] RM_API static LaneProfile highway();

  /// Historical alias of two_lane_rural().
  [[nodiscard]] RM_API static LaneProfile two_lane_default();
};

/// Optional locked end headings [rad] for fit_clothoid_path /
/// author_clothoid_road: interior headings stay spline-estimated while a
/// locked end is interpolated exactly (G1 Hermite). The Create Road tool
/// locks these to snap continuation headings so chained roads join G1
/// within rm::tol (02_editing_tools.md §2).
struct EndpointHeadings {
  std::optional<double> start;
  std::optional<double> end;
};

/// Fits a G1 clothoid reference line through `waypoints` (headings
/// estimated by the Clothoids G1 spline). Consecutive joints are G1:
/// position and heading continuous within rm::tol. This is the same fit
/// the authoring API and the node-edit commands use.
///
/// Errors (InvalidArgument): fewer than 2 waypoints, coincident consecutive
/// waypoints, or a failed spline fit.
[[nodiscard]] RM_API Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints);

/// Fits a G1 Hermite clothoid path: one clothoid per consecutive waypoint
/// pair interpolating the given heading [rad] at every waypoint
/// (headings.size() == waypoints.size()). Sampling a pure line/arc/spiral
/// chain at its record boundaries and re-fitting with its own headings
/// reproduces the chain (the G1 Hermite interpolant of each segment's end
/// poses is unique) — the derivation re-fit of foreign roads (edit
/// operations, 01 §2.5) relies on this.
///
/// Errors (InvalidArgument): the point-only overload's errors, or a heading
/// count mismatch.
[[nodiscard]] RM_API Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints,
                                                               std::span<const double> headings);

/// Fits with the point-only overload's estimated headings, then re-fits
/// G1 Hermite with any locked end heading substituted — so a locked end
/// matches exactly while the interior keeps the estimated flow. Both ends
/// unlocked is exactly the point-only overload.
///
/// Errors (InvalidArgument): the point-only overload's errors.
[[nodiscard]] RM_API Expected<ReferenceLine> fit_clothoid_path(std::span<const Waypoint> waypoints,
                                                               const EndpointHeadings& locked);

/// Fits a G1 clothoid path through `waypoints` (end headings locked per
/// `locked`), creates a road with `profile`, and inserts it into the
/// network. The waypoints are recorded as Road::authoring_waypoints.
///
/// Errors (InvalidArgument): the fit_clothoid_path errors, an odr_id
/// already in use, or an empty lane profile.
[[nodiscard]] RM_API Expected<RoadId> author_clothoid_road(RoadNetwork& network,
                                                           std::span<const Waypoint> waypoints,
                                                           const LaneProfile& profile,
                                                           std::string name = {},
                                                           std::string odr_id = {},
                                                           const EndpointHeadings& locked = {});

} // namespace roadmaker
