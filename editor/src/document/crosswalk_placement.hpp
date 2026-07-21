#pragma once

// Crosswalk placement helper (p3-s3, issue #222), shared by the interactive
// Crosswalk & Stop Line tool (GW-5 step 5) and the Library drag-drop path
// (GW-2 step 10) so both funnel through identical geometry and identical undo
// semantics. Pure translation over the kernel's junction marking generators —
// no widgets, no kernel changes — so it is unit-testable headless.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// A crosswalk drop/click snaps to a junction approach whose reference line
/// passes within this lateral distance [m] of the cursor — wide enough to grab
/// the arm by aiming anywhere across its carriageway, tight enough that a drop
/// in open space is rejected. Shared by the tool and the Library drop so the
/// two agree on where an approach is in reach.
inline constexpr double kCrosswalkSnapThreshold = 12.0;

/// The crosswalk to place on ONE junction approach, filtered from the all-arms
/// generator: calls edit::junction_crosswalks against `network` and keeps the
/// object whose owning road is `arm_road`. Returns a {owning road, object} pair
/// ready for edit::add_object; nullopt when the arm has no crossing or the
/// junction is stale/foreign.
///
/// The companion stop line is NOT an object any more (p4-s3, #318): the arm
/// already has a derived one, and the caller links the two by pushing
/// edit::set_stopline_distance inside the same undo macro. That also retires
/// the odr-id re-numbering this helper used to do — with a single generator
/// left there is no second batch to collide with.
[[nodiscard]] std::optional<std::pair<RoadId, Object>>
crosswalk_for_arm(const RoadNetwork& network,
                  JunctionId junction,
                  RoadId arm_road,
                  const edit::CrosswalkParams& params);

/// A junction approach resolved from a world-space cursor: the junction, the
/// arm (approach) road, the world point where that arm meets the junction, and
/// the heading pointing INTO the junction there (for the chevron affordance).
struct ArmHit {
  JunctionId junction;
  RoadId arm_road;
  /// Which end of `arm_road` touches the junction — with `arm_road` this is the
  /// RoadEnd that identifies the approach's stop line (p4-s3, #318).
  ContactPoint contact = ContactPoint::End;
  double anchor_x = 0.0;
  double anchor_y = 0.0;
  double heading = 0.0; ///< [rad] into the junction at the arm end
};

/// The nearest junction approach to (x, y) whose reference line passes within
/// `threshold` [m] — the drag-drop path has no PickHit, so it resolves the arm
/// from the cursor exactly as the tool does. Skips junction connecting roads
/// (road->junction valid): only an incoming approach can carry a crosswalk.
/// A road is an approach when one of its ends is recorded as a junction arm
/// (edit::junction_at_end); the end nearer the cursor wins. nullopt when no
/// approach is close enough.
[[nodiscard]] std::optional<ArmHit>
nearest_junction_arm(const RoadNetwork& network, double x, double y, double threshold);

} // namespace roadmaker::editor
