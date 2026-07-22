// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/edit/command.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/road/authoring.hpp"

#include <memory>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

/// Parametric intersection assemblies: one command that lays down the stub
/// roads of a standalone junction and generates its connecting roads, as a
/// single undo unit. Built on the M2 topology primitives (create_road +
/// create_junction) so the output is exactly what the interactive tools
/// produce — validator-clean, apply→revert byte-identical.
namespace roadmaker::edit::assembly {

/// A 2D placement in the kernel frame: position [m] and heading [rad],
/// right-handed / Z-up (OpenDRIVE convention). The junction center sits at
/// (x, y); `heading` orients the "through" road (the T's crossbar / the X's
/// first axis).
struct Pose {
  double x = 0.0;
  double y = 0.0;
  double heading = 0.0;

  friend bool operator==(const Pose&, const Pose&) = default;
};

/// Tuning for the intersection assemblies (mirrors the plain-aggregate style
/// of TAttachOptions / JunctionGenOptions).
struct IntersectionParams {
  /// Length [m] of each stub road beyond the junction area (arm_length_m > 0).
  double arm_length_m = 30.0;

  /// Half-size [m] of the junction area: each arm's inner end sits this far
  /// from the center, leaving room for the generated turns. 0 = auto — the
  /// larger of the profile half-width and generation.min_turn_radius_m (each
  /// + 1 m), matching how attach_t_junction sizes its gap.
  double gap_m = 0.0;

  /// Cross section applied to every arm (defaults to the two-lane rural
  /// template — the Create Road default).
  LaneProfile profile = LaneProfile::two_lane_rural();

  /// Passed through to the connecting-road generator.
  JunctionGenOptions generation;
};

/// A 3-way T-intersection centered at `pose`: a straight "through" road along
/// `pose.heading` (two opposite arms) plus one perpendicular stem arm (to the
/// left of the through direction). Lays down three stub roads whose inner ends
/// stop `gap` short of the center, then generates the junction from them —
/// ONE command (apply→revert byte-identical). Errors (invalid_command):
/// arm_length_m ≤ 0, an empty lane profile, or a create_junction failure
/// (e.g. arms too tight for the min turn radius) surfaced at apply.
[[nodiscard]] RM_API std::unique_ptr<Command>
t_intersection(const RoadNetwork& network, Pose pose, IntersectionParams params = {});

/// A 4-way X-intersection centered at `pose`: four arms at `pose.heading`,
/// +90°, +180°, +270°. Same construction and guarantees as t_intersection.
[[nodiscard]] RM_API std::unique_ptr<Command>
x_intersection(const RoadNetwork& network, Pose pose, IntersectionParams params = {});

/// Forms a 4-way junction where two EXISTING roads `a` and `b` cross — the
/// two-road crossing assembly (unlike cross_onto_road, which builds its own
/// perpendicular stems from a single target). Computes the plan-view crossing
/// (road_intersections), splits each road at its crossing station ± gap,
/// deletes the two middle stubs (the junction area), then generates a common
/// junction from the four resulting ends — ONE command (apply→revert
/// byte-identical). Errors (invalid_command): the roads do not cross interior
/// to both, a road already participates in a junction, or a paramPoly3 record
/// at a cut (M2 split restrictions), surfaced at apply.
[[nodiscard]] RM_API std::unique_ptr<Command>
cross_roads(const RoadNetwork& network, RoadId a, RoadId b, IntersectionParams params = {});

/// Tees a new perpendicular stem road INTO the side of `target` at station `s`
/// — the on-road drop of a T assembly (gate finding 1). Projects the drop onto
/// the road, aligns the stem to the road tangent (perpendicular, to the left),
/// and attaches it (split + junction) in ONE command, instead of dropping a
/// floating standalone junction at the cursor. Errors (invalid_command): a stale
/// target, a non-positive arm length, a drop too near a road end, or the
/// underlying attach_t_junction restrictions (target already in a junction, a
/// paramPoly3 record at the cut) surfaced at apply.
[[nodiscard]] RM_API std::unique_ptr<Command>
tee_onto_road(const RoadNetwork& network, RoadId target, double s, IntersectionParams params = {});

/// Crosses a 4-way junction OVER `target` at station `s` — the on-road drop of
/// an X assembly (v1). Splits the target at s±gap (its two halves are the
/// collinear through arms, collinear BY CONSTRUCTION), removes the middle stub
/// as the junction area, lays two perpendicular stems (left + right), and
/// generates the junction from the four ends. Inherits split's restrictions
/// (no paramPoly3 at the cut, the target not already in a junction) as explicit
/// errors surfaced at apply; ONE command (apply→revert byte-identical).
[[nodiscard]] RM_API std::unique_ptr<Command> cross_onto_road(const RoadNetwork& network,
                                                              RoadId target,
                                                              double s,
                                                              IntersectionParams params = {});

} // namespace roadmaker::edit::assembly
