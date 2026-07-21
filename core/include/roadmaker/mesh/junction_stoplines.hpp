#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <string>
#include <vector>

namespace roadmaker {

/// Default setback [m] from the junction mouth to the near face of a stop line.
/// The value the pre-p4-s3 `edit::junction_stop_lines` generator used, kept so
/// files authored before the entity existed reload unchanged.
inline constexpr double kStopLineDefaultDistance = 4.0;

/// Extent [m] of the painted band ALONG the road. Note the axis convention this
/// shares with the old generator and with the exported object: `@length` is the
/// (thin) along-road extent and `@width` spans ACROSS the lanes — inverted with
/// respect to the crosswalk object, whose band runs across the road.
inline constexpr double kStopLineThickness = 0.3;

/// The solved geometry of one junction stop line — the single source shared by
/// the mesher, the writer's object materialization, the editor's StopLine tool
/// and properties pane, and the Python bindings (p4-s3, issue #318). Plan-view
/// (x, y) in the kernel frame; the band is painted on the road surface, so it
/// carries no z of its own.
///
/// Plain data, no out-of-line member functions: a member defined in the .cpp
/// would need its own RM_API under RM_BUILD_SHARED=ON (see
/// JunctionCornerInfo::apex), and exporting the struct wholesale would trip
/// MSVC C4251 on its std::string member.
struct JunctionStopLineInfo {
  /// Identity — the junction-facing end of the arm road, matching StopLine::arm.
  RoadEnd arm;

  /// Effective setback [m] from the junction mouth, clamped to
  /// `[0, max_distance]`. Equals `kStopLineDefaultDistance` when nothing is
  /// authored.
  double distance = 0.0;

  /// Largest setback [m] the arm road leaves room for — `length - thickness`.
  /// The Distance spin box's upper bound.
  double max_distance = 0.0;

  /// false ⇒ the band spans the approach (incoming) lanes; true ⇒ the outgoing
  /// lanes.
  bool flipped = false;

  /// True when a StopLine record for this arm supplied `distance` (as opposed
  /// to the default).
  bool distance_authored = false;

  /// True when the junction carries a StopLine record for this arm at all —
  /// i.e. something (distance, flip, or a crosswalk link) is authored. Drives
  /// the "Reset to default" affordance.
  bool authored = false;

  /// The odr id of the crosswalk this line was placed alongside, empty when
  /// free-standing.
  std::string crosswalk_odr_id;

  /// Centre of the band in the arm road's track coordinates: `s_center` is the
  /// station of its mid-thickness, `t_center` the mid-span lateral offset.
  /// These are exactly the `@s`/`@t` of the exported object.
  double s_center = 0.0;
  double t_center = 0.0;

  /// Span ACROSS the lanes [m] — the exported `@width`.
  double span = 0.0;

  /// Extent ALONG the road [m] — the exported `@length`, always
  /// kStopLineThickness.
  double thickness = kStopLineThickness;

  /// The band's centreline endpoints in world coordinates, left (larger t) and
  /// right (smaller t) of the arm's reference line. The tool picks against the
  /// segment between them.
  std::array<double, 2> left{};
  std::array<double, 2> right{};
};

/// Solves every stop line of `junction_id`: one per distinct arm whose
/// junction-facing end has driving lanes in the line's direction, in the
/// junction's connection order. Authored StopLine records are merged over the
/// derivation; a record whose arm is not (or no longer) an arm of this junction
/// is ignored.
///
/// Returns empty for a stale id or a junction with no solvable arm. An arm
/// carrying a live untagged `signalLines` object on its near half is SUPPRESSED
/// — a legacy or foreign file already draws its own stop line there, and
/// deriving a second one would double-draw it.
[[nodiscard]] RM_API std::vector<JunctionStopLineInfo>
junction_stoplines(const RoadNetwork& network, JunctionId junction_id);

} // namespace roadmaker
