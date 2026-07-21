#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/junction.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <numbers>
#include <string>
#include <vector>

namespace roadmaker {

/// Half-width [rad] of the STRAIGHT band: a movement deflecting the arm-face
/// heading by no more than this reads as "straight ahead" (30 degrees).
///
/// Deliberately NOT the planner's 10-degree lane-discipline threshold
/// (operations.cpp, plan_junction): that one decides which LANES a movement may
/// use and errs tiny on purpose, because a movement that even slightly turns
/// left must not cross the inner lanes' through connections. This one is a
/// PERCEPTUAL label shown to the author, so the bands are the ones a driver
/// would name. Changing either must not change the other — see the matching
/// comment at the planner's threshold.
inline constexpr double kManeuverStraightThreshold = 30.0 * std::numbers::pi / 180.0;

/// Lower bound [rad] on the deflection that reads as a U-TURN (150 degrees).
/// A movement returning to the arm it came from is a U-turn whatever its
/// deflection, so this only classifies hairpins between DIFFERENT arms.
inline constexpr double kManeuverUTurnThreshold = 150.0 * std::numbers::pi / 180.0;

/// One face's endpoint-slide constraint line: the anchor-lane cross-section
/// segment a maneuver endpoint may be dragged along (p4-s6, issue #227).
///
/// The offsets are signed along the arm's +t axis (positive = left of the arm's
/// reference line) and measured from the anchor lane's INNER boundary, which is
/// `anchor` and offset 0 — exactly the anchoring the generator uses, so an
/// unauthored maneuver sits at offset 0 on both faces. `min_point`/`max_point`
/// are those bounds in world coordinates, for the tool's constraint line.
struct ManeuverSlide {
  std::array<double, 2> anchor{};
  std::array<double, 2> min_point{};
  std::array<double, 2> max_point{};
  double min_offset = 0.0;
  double max_offset = 0.0;
};

/// One maneuver of a junction — one connecting road's path through it, with the
/// derived turn type merged with whatever is authored (p4-s6, issue #227). The
/// single source shared by the editor's Maneuver tool and properties rows, the
/// command layer's validate-first checks, the Python bindings, and (later) the
/// turn-lane arrows of p4-s7, so none of them can disagree about what a
/// maneuver is or where it may be dragged.
///
/// Plain data, no out-of-line member functions: a member defined in the .cpp
/// would need its own RM_API under RM_BUILD_SHARED=ON, and exporting the struct
/// wholesale would trip MSVC C4251 on its std::string and std::vector members.
struct JunctionManeuverInfo {
  /// Identity — the connecting road, matching Maneuver::road. Stable across
  /// regeneration: a surviving turn keeps its RoadId.
  RoadId road;

  /// The connecting road's OpenDRIVE id, for the panel's "Turn <id>" label.
  std::string road_odr_id;

  /// The arm face the maneuver leaves, and the one it enters. Read off the
  /// connecting road's own predecessor/successor links (§12.4.2: a connecting
  /// road runs in driving direction, start at the incoming road).
  RoadEnd from;
  RoadEnd to;

  /// The linked lane odr ids on `from` and `to`.
  int from_lane = 0;
  int to_lane = 0;

  /// The turn type derived from the arm-face headings.
  TurnType computed = TurnType::Straight;

  /// What consumers should show and act on: the override when there is one,
  /// else `computed`.
  TurnType effective = TurnType::Straight;

  /// True when a Maneuver record supplies `effective` (as opposed to the
  /// derivation). Drives the panel's "revert to computed" affordance.
  bool overridden = false;

  /// True when the record locks this road's geometry against regeneration.
  bool locked = false;

  /// True when the junction carries a Maneuver record for this road at all.
  bool authored = false;

  /// True when the maneuver returns to the arm it came from — an EXPLICIT
  /// U-turn, which the planner never emits (it skips the i == j pair) and which
  /// therefore exists only because it was authored. It has no derived geometry
  /// to fall back on, so it cannot be reset, only deleted.
  bool is_uturn_explicit = false;

  /// The fixed end directions [rad] the path is refitted against: the tangent
  /// leaving the incoming face and the tangent entering the outgoing face.
  double start_heading = 0.0;
  double end_heading = 0.0;

  /// Effective endpoint slides [m] (0 when unauthored), and the segments they
  /// slide along.
  double start_offset = 0.0;
  double end_offset = 0.0;
  ManeuverSlide start_slide;
  ManeuverSlide end_slide;

  /// The authored INTERIOR waypoints, empty for a derived path.
  std::vector<Waypoint> control_points;

  /// The connecting road's sampled centerline with elevation — the render
  /// polyline and the tool's pick polyline.
  std::vector<std::array<double, 3>> path;
};

/// Every maneuver of `junction_id`, in the junction's CONNECTION order.
///
/// Walks `Junction::connections` rather than the arms on purpose: a FOREIGN
/// junction (read from someone else's file, so it has no arm list and cannot be
/// regenerated) still has connections, and its maneuvers must still be readable
/// — inspectable and labelled, just not authorable. Connections whose
/// connecting road is stale, geometry-less, or not linked to two live roads are
/// skipped, exactly as the mesher skips them.
///
/// Returns empty for a stale id and, naturally, for a SPAN (virtual) junction:
/// §12.7 junctions never cut the main road, so they carry no connections.
///
/// `sampling` controls the density of `path` only.
[[nodiscard]] RM_API std::vector<JunctionManeuverInfo> junction_maneuvers(
    const RoadNetwork& network, JunctionId junction_id, const SamplingOptions& sampling = {});

/// The turn type of a movement deflecting the arm-face heading by `delta`
/// [rad], as classified by kManeuverStraightThreshold / kManeuverUTurnThreshold.
/// `same_arm` (a movement returning to the arm it came from) is always a U-turn.
/// Exposed so the tool and the tests classify with the query, not a copy of it.
[[nodiscard]] RM_API TurnType maneuver_turn_type(double delta, bool same_arm);

} // namespace roadmaker
