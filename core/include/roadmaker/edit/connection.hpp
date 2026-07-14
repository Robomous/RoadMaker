#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// The connection engine (gate-extension WS-2): the single authority for the
/// contact/fit geometry that junction generation, assembly-on-road drops, and
/// end-gap closing all share. The pure primitives here were previously trapped
/// in operations.cpp's anonymous namespace; operations.cpp now calls them so
/// the three consumers cannot drift. New code should target this API rather
/// than re-deriving contact frames or clothoid fits.
namespace roadmaker::edit {

class Command;

/// Which side of a road a perpendicular stem leaves from (the T-stem sense):
/// Left rotates the road tangent by +pi/2, Right by -pi/2.
enum class Side {
  Left,
  Right,
};

/// A planar pose in the inertial frame [m, rad].
struct Pose2D {
  double x = 0.0;
  double y = 0.0;
  double heading = 0.0;
};

/// The junction-facing state of a road end — the successor of the old ArmEnd,
/// with the plan-view curvature and elevation a G2 weld needs. `into_hdg` is
/// the tangent leaving the arm INTO the junction (a connecting road's start
/// heading here); `out_hdg` is the tangent entering the arm OUT of the junction
/// (a connecting road's end heading here); `road_hdg` is the arm's own +s
/// heading at the station, the axis lateral offsets are measured against. All
/// angles [rad] in the inertial frame; `curvature` is the plan-view curvature
/// [1/m] at the end, signed along the arm's +s direction.
struct ContactState {
  double x = 0.0;
  double y = 0.0;
  double into_hdg = 0.0;
  double out_hdg = 0.0;
  double road_hdg = 0.0;
  double curvature = 0.0;
  double z = 0.0;
  double grade = 0.0; ///< dz/ds along the arm's +s at the station.
  LaneSectionId section;
  double station = 0.0;
};

/// A driving lane at an arm's contact end, with the lateral offset of its INNER
/// boundary (nearer lane 0, laneOffset included, positive = left of the arm's
/// reference line). A connecting road anchors its reference line on that
/// boundary so linked lanes coincide (smooth_fit, §10.3). The successor of the
/// old ArmLane.
struct ContactLane {
  int odr_id = 0;
  double width = 0.0;
  double inner_t = 0.0;
};

/// One end of a connector to fit: pose plus the plan-view curvature and
/// elevation (z, grade) to honour there.
struct ConnectorEndpoint {
  double x = 0.0;
  double y = 0.0;
  double heading = 0.0;
  double curvature = 0.0;
  double z = 0.0;
  double grade = 0.0;
};

/// Fit controls for fit_connector.
struct ConnectorParams {
  /// A fitted path longer than this multiple of the straight-line endpoint
  /// distance is rejected as a loop (0 disables the check).
  double max_loop_factor = 4.0;
  /// Reject a fit whose minimum radius of curvature falls below this [m]
  /// (0 disables the check).
  double min_turn_radius_m = 0.0;
  /// Match the endpoint curvatures too (G2, via the Clothoids three-arc
  /// interpolant) so an arc starting at a weld shows no kink (finding 3). When
  /// false, only position + heading are matched (G1) — reproducing the M2
  /// junction connector fit exactly (the junction path keeps g2=false).
  bool g2 = false;
};

/// A fitted connector: the driving-direction reference line plus the elevation
/// Hermite matching the endpoints' z and grade (empty when both ends are flat).
struct Connector {
  ReferenceLine line;
  std::vector<Poly3> elevation;
};

/// Options for close_gap.
struct CloseGapOptions {
  /// Ends farther apart than this [m] are refused.
  double max_gap_m = 30.0;
  /// Below this gap [m] the two ends are treated as coincident and closed by a
  /// pure link; a wider gap is bridged with a connector road.
  double coincident_gap_m = 0.05;
};

/// The post-regeneration coincidence report: the maxima between each connecting
/// road's ends and the arms they link, computed with the SAME anchor math
/// plan_junction uses so checker and generator cannot drift. `breaches` is true
/// when position or heading exceeds tol::kWeldPosition / kWeldHeading; the
/// curvature gap is reported for information only, since a G1 weld legitimately
/// leaves a curvature step (only a G2 close_gap drives it below kWeldCurvature).
struct WeldReport {
  double max_position_gap = 0.0;  ///< [m]
  double max_heading_gap = 0.0;   ///< [rad]
  double max_curvature_gap = 0.0; ///< [1/m]
  bool breaches = false;
};

/// The tangent pose at station `s` on `road`. With a `branch` side the heading
/// is rotated +/-pi/2 so a perpendicular stem can leave the road there (finding
/// 1's aligned assembly drop).
[[nodiscard]] RM_API Expected<Pose2D>
aligned_pose_on_road(const RoadNetwork& network, RoadId road, double s, std::optional<Side> branch);

/// The contact state at a road end (stale/empty ids are InvalidArgument).
[[nodiscard]] RM_API Expected<ContactState> contact_state(const RoadNetwork& network,
                                                          const RoadEnd& end);

/// Driving lanes at an arm's contact end, curb-in (outermost first). `incoming`
/// selects the lanes leading INTO the junction, else those leading OUT.
[[nodiscard]] RM_API std::vector<ContactLane> driving_lanes_at(const RoadNetwork& network,
                                                               const RoadEnd& end,
                                                               const ContactState& contact,
                                                               bool incoming);

/// Inertial point at lateral offset `t` (positive = left) from a contact state.
[[nodiscard]] RM_API std::array<double, 2> contact_lateral(const ContactState& contact, double t);

/// Fit a connector between two endpoints. G1 (position + heading) via the
/// straight-fillet-straight guide + clothoid chain (see ConnectorParams for the
/// G2 follow-up). Errors (InvalidArgument): the clothoid fit failed, the path
/// loops, or it breaches min_turn_radius_m.
[[nodiscard]] RM_API Expected<Connector> fit_connector(const ConnectorEndpoint& a,
                                                       const ConnectorEndpoint& b,
                                                       const ConnectorParams& params);

/// The junction that already owns `end` as an arm, if any.
[[nodiscard]] RM_API std::optional<JunctionId> junction_at_end(const RoadNetwork& network,
                                                               const RoadEnd& end);

/// The junction whose recorded arm set is exactly `ends` (order-free), if any —
/// so a repeat selection regenerates in place instead of overlaying a second
/// junction (finding 5).
[[nodiscard]] RM_API std::optional<JunctionId> matching_junction(const RoadNetwork& network,
                                                                 std::span<const RoadEnd> ends);

/// Verify a junction's connecting roads still coincide with the arms they link.
[[nodiscard]] RM_API Expected<WeldReport> verify_junction_welds(const RoadNetwork& network,
                                                                JunctionId junction);

/// Whether two free road ends can be linked/gap-closed (mirrors check_mergeable:
/// both ends free, not in a junction, within max_gap_m). An enablement query
/// for the editor's "Link Ends" affordance.
[[nodiscard]] RM_API Expected<void> check_linkable(const RoadNetwork& network,
                                                   const RoadEnd& a,
                                                   const RoadEnd& b,
                                                   const CloseGapOptions& options = {});

/// Close the gap between two free road ends: link them (near-coincident) or
/// bridge them with a G2 connector road, in one undoable command. A refused
/// precondition yields a command whose apply() reports the error.
[[nodiscard]] RM_API std::unique_ptr<Command> close_gap(const RoadNetwork& network,
                                                        const RoadEnd& a,
                                                        const RoadEnd& b,
                                                        const CloseGapOptions& options = {});

} // namespace roadmaker::edit
