#pragma once

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/id.hpp"

#include <optional>
#include <vector>

namespace roadmaker {

/// OpenDRIVE lane types RoadMaker distinguishes in M1. Anything else parses
/// as `Other` (with a diagnostic), never dropped.
enum class LaneType {
  Driving,
  Stop,
  Shoulder,
  Biking,
  Sidewalk,
  Border,
  Restricted,
  Parking,
  Median,
  Curb,
  None,
  Other,
};

/// Lane marking types RoadMaker renders in M1; exotic ones map to Other
/// (with a diagnostic).
enum class RoadMarkType {
  None,
  Solid,
  Broken,
  SolidSolid,
  SolidBroken,
  BrokenSolid,
  Other,
};

/// One <roadMark> record on a lane boundary.
struct RoadMark {
  /// Start offset LOCAL to the owning lane section [m].
  double s_offset = 0.0;

  RoadMarkType type = RoadMarkType::None;

  /// Painted width [m]; OpenDRIVE default when absent.
  double width = 0.12;
};

/// A single lane within a lane section.
///
/// OpenDRIVE lane numbering: 0 is the (width-less) center lane on the
/// reference line, positive ids grow to the LEFT of the travel direction,
/// negative ids to the RIGHT, ordered outward.
struct Lane {
  /// Owning section (back-reference).
  LaneSectionId section;

  /// Signed OpenDRIVE lane id within the section.
  int odr_id = 0;

  LaneType type = LaneType::None;

  /// Width polynomials. Poly3::s is the sOffset LOCAL to the owning lane
  /// section's start (per OpenDRIVE), sorted ascending. Empty for lane 0.
  std::vector<Poly3> widths;

  /// Markings on this lane's OUTER boundary, sorted by s_offset (section-
  /// local). On lane 0 this is the center line marking.
  std::vector<RoadMark> road_marks;

  /// OpenDRIVE lane ids of the linked lanes in the previous/next lane
  /// section (or road, across road boundaries).
  std::optional<int> predecessor;
  std::optional<int> successor;
};

} // namespace roadmaker
