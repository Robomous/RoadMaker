#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/lane.hpp"

#include <optional>
#include <vector>

namespace roadmaker {

/// One lane of a road style's cross section.
///
/// Unlike the authoring-time `LaneSpec` (constant `double` width, a single
/// `bool outer_marking` that only ever paints a solid line), a `StyleLane`
/// carries a full `Poly3` width and an optional full `RoadMark` — so a style
/// can round-trip a poly-width (carved) lane and express a dashed, coloured, or
/// multi-line boundary mark. A style is a *delta applied to an existing road*
/// (edit::apply_road_style), not a creation blueprint.
struct StyleLane {
  LaneType type = LaneType::Driving;

  /// Width polynomial, sOffset local to the (single) section — a constant lane
  /// leaves `b/c/d` at zero. Must evaluate positive over the section.
  Poly3 width{.a = 3.5};

  /// Mark painted on this lane's OUTER boundary (the one farther from the
  /// reference line). nullopt paints nothing. For the innermost same-direction
  /// lane this boundary is the divider to the next lane out — a dashed white
  /// mark here is the same-direction lane line (#194).
  std::optional<RoadMark> outer_mark;
};

/// A serializable cross-section style applied to an existing road
/// (edit::apply_road_style). Replaces the road's lane profile and boundary
/// marks; everything orthogonal to the cross section (reference-line geometry,
/// elevation, superelevation, junction connectivity, name, placed objects and
/// signals) is preserved by the command.
struct RoadStyle {
  /// Lanes left of the reference line, innermost (+1) first.
  std::vector<StyleLane> left;

  /// Lanes right of the reference line, innermost (-1) first.
  std::vector<StyleLane> right;

  /// Mark on lane 0 (the center line). nullopt paints nothing.
  std::optional<RoadMark> center_mark;

  // Starter styles the Library ships. Contents are content-tested — changing a
  // style is a visible product change, not a refactor.

  /// Urban two-lane-per-direction: two 3.5 m driving lanes each way, a dashed
  /// white line between the same-direction lanes and a solid white edge line,
  /// with a solid yellow center line (#194).
  [[nodiscard]] RM_API static RoadStyle urban_two_lane();

  /// Two-lane rural: one 3.5 m driving lane each way with solid white edge
  /// lines and a right-hand shoulder, broken white center line.
  [[nodiscard]] RM_API static RoadStyle two_lane_rural();

  /// Highway: two 3.75 m driving lanes each way with a dashed white lane line,
  /// a solid white edge line and a wide shoulder; no center marking.
  [[nodiscard]] RM_API static RoadStyle highway();
};

} // namespace roadmaker
