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

/// Lane travel direction relative to the reference-line-derived standard
/// (e_lane_direction). Introduced in OpenDRIVE 1.8.0, so it is legal under
/// both writer targets (1.8.1 and 1.9.0) with no version gating. The
/// enumeration is exactly standard | reversed | both — there is NO `same`
/// literal (1.8.1 §11 + Annex A.3.10 Table 173; 1.9.0 §11.2/§11.3.1 + Annex
/// A.3.11 Table 180). `Standard` means "determined by the <left>/<right>
/// grouping and the road @rule (LHT/RHT)"; the @direction attribute overrides
/// that. Written explicitly only when not Standard (byte-stable default).
enum class LaneDirection {
  Standard,
  Reversed,
  Both,
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

/// e_roadMarkColor (OpenDRIVE 1.9.0 §11.9, Table 48). Standard resolves to
/// white or yellow by mark type/region in the renderer (the kernel stays
/// render-free). Unknown spellings parse as Other with a diagnostic — never
/// dropped, mirroring RoadMarkType::Other.
enum class RoadMarkColor {
  Standard,
  White,
  Yellow,
  Red,
  Blue,
  Green,
  Orange,
  Other,
};

/// One painted stripe of a (possibly multi-line) road mark — a <line> inside
/// the <roadMark>'s <type> element (§11.9.1, Table 50). Populated only for
/// multi-line marks (solid_solid etc.); empty for the common single-stripe
/// case where RoadMark::width is authoritative (M2 behaviour, byte-stable).
struct RoadMarkLine {
  double width = 0.12;   ///< stripe width [m]
  double length = 0.0;   ///< painted length [m] (0 = continuous)
  double space = 0.0;    ///< gap length [m] (0 = solid)
  double t_offset = 0.0; ///< lateral offset of this stripe from the mark line [m]
  double s_offset = 0.0; ///< longitudinal start offset within the mark [m]

  friend bool operator==(const RoadMarkLine&, const RoadMarkLine&) = default;
};

/// One <roadMark> record on a lane boundary.
struct RoadMark {
  /// Start offset LOCAL to the owning lane section [m].
  double s_offset = 0.0;

  RoadMarkType type = RoadMarkType::None;

  /// Painted width [m]; OpenDRIVE default when absent.
  double width = 0.12;

  /// e_roadMarkColor (§11.9). Written explicitly only when not Standard.
  RoadMarkColor color = RoadMarkColor::Standard;

  /// Explicit multi-line geometry (<type>/<line>, §11.9.1). Empty for the
  /// simple single-stripe mark; populated (two stripes with symmetric
  /// t_offset) for solid_solid / solid_broken / broken_solid so the mark
  /// renders as true dual geometry instead of one strip.
  std::vector<RoadMarkLine> lines;

  friend bool operator==(const RoadMark&, const RoadMark&) = default;
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

  /// Travel direction (e_lane_direction, §11). Defaults to Standard, which
  /// emits nothing on write; only Reversed/Both are serialized.
  LaneDirection direction = LaneDirection::Standard;

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
