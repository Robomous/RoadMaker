#pragma once

// Vertical-profile helpers for the profile panel (hardening sprint WS-C,
// docs/design/hardening/t_junction.md sibling workstream): plan-view crossing
// detection and the overpass (grade-separation) profile builder. Pure
// functions over the kernel network — headless testable, no Qt.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <vector>

namespace roadmaker::editor::elevation {

/// A plan-view intersection of `road` with another road's reference line.
struct Crossing {
  RoadId other;
  double s_self = 0.0;  ///< station on the queried road
  double s_other = 0.0; ///< station on the other road
};

/// Plan-view crossings of `road`'s reference line with every other road's
/// (sampled segment intersection, ~1 m resolution). Junction connecting
/// roads are skipped (their vertical design belongs to the junction
/// surface), as are crossings within `end_margin` of either road's ends —
/// those are junction territory, not overpass territory.
[[nodiscard]] std::vector<Crossing>
find_crossings(const RoadNetwork& network, RoadId road, double end_margin = 10.0);

/// Profile nodes expressing "cross over (or under) every crossing road with
/// `clearance` meters between the surfaces": the road's CURRENT profile
/// nodes plus, per crossing, a locked-grade node at the crossing station
/// (z_other ± clearance) and approach/departure nodes `ramp` meters away
/// carrying the original elevation. `ramp` is sized so the connecting grade
/// stays within `max_grade` (longer when the hump is taller). Existing
/// nodes inside a ramp span are dropped — the hump owns that interval.
[[nodiscard]] std::vector<edit::ElevationPoint> overpass_points(const RoadNetwork& network,
                                                                RoadId road,
                                                                bool over,
                                                                double clearance = 5.5,
                                                                double max_grade = 0.08);

} // namespace roadmaker::editor::elevation
