#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <optional>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// Snapping queries (docs/m2/01_editing_framework.md §6): pure functions the
/// editing tools call per cursor move; the viewport only renders the returned
/// hint (marker + ghost tangent). All coordinates are kernel plan-view
/// meters (right-handed, Z-up); headings are CCW from +X in radians.
namespace roadmaker::edit {

enum class SnapKind {
  Grid,
  RoadEndpoint,
  TangentContinuation,
};

struct SnapOptions {
  /// Capture radius [m] (screen-scaled by the caller). Candidates at exactly
  /// this distance still snap.
  double radius = 2.0;

  /// Grid spacing [m]; nullopt or a non-positive value disables grid
  /// snapping.
  std::optional<double> grid;

  bool endpoints = true;
  bool tangent = true;
};

struct SnapResult {
  Waypoint position;

  /// Continuation heading [rad], pointing AWAY from the source road — the
  /// start heading a chained road needs for G1 continuity. Set only for
  /// TangentContinuation: equal to evaluate(length).hdg at a road's end,
  /// and to that heading plus pi (wrapped to [-pi, pi]) at its start.
  std::optional<double> heading;

  SnapKind kind = SnapKind::Grid;

  /// Source road for RoadEndpoint / TangentContinuation.
  std::optional<RoadId> road;
};

/// Best snap candidate for `cursor`, or nullopt when nothing is in range.
/// Priority on conflict: RoadEndpoint > TangentContinuation > Grid; within
/// a kind the candidate closest to the cursor wins (ties keep the first
/// road in arena slot order — deterministic). Tangent continuation projects
/// the cursor onto the tangent ray extending each road end beyond the road;
/// at or behind the end itself only endpoint/grid snapping applies.
/// Degenerate roads (reference line shorter than rm::tol::kLength) are
/// ignored.
[[nodiscard]] RM_API std::optional<SnapResult>
snap_point(const RoadNetwork& network, Waypoint cursor, const SnapOptions& options);

} // namespace roadmaker::edit
