// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <string>
#include <vector>

namespace roadmaker {

/// One connecting road's contribution to a junction's floor — its "surface
/// span" (p4-s5, issue #320). The single source shared by the floor mesher, the
/// editor's Junction Surface tool and properties rows, the command layer's
/// solvability check, and the Python bindings, so none of them can disagree
/// about what a span is or where its samples fall.
///
/// NOT related to `SpanArm`, a VIRTUAL junction's s-interval (§12.7): surface
/// spans exist only on ordinary arm junctions, which are the only junctions
/// with a floor.
///
/// Plain data, no out-of-line member functions: a member defined in the .cpp
/// would need its own RM_API under RM_BUILD_SHARED=ON, and exporting the struct
/// wholesale would trip MSVC C4251 on its std::string and std::vector members.
struct JunctionSurfaceSpanInfo {
  /// Identity — the connecting road, matching SurfaceSpan::road. Stable across
  /// regeneration: `retarget_junction` matches surviving turns by TurnKey and
  /// rewrites their geometry onto the SAME RoadId.
  RoadId road;

  /// The connecting road's OpenDRIVE id, for the panel's "Turn <id>" label.
  std::string road_odr_id;

  /// Effective Include Samples: false ⇒ this span's samples leave the fill
  /// inputs (elevation Dirichlet, centerline constraints, boundary-debris
  /// protection). Its footprint stays in the union either way, so the floor's
  /// coverage and the exported `<boundary>` are unaffected.
  bool included = true;

  /// Effective sort index — higher wins where span footprints overlap.
  int sort_index = 0;

  /// True when the junction carries a SurfaceSpan record for this road. Drives
  /// the panel's "authored" affordance.
  bool authored = false;

  /// The raw plan-view footprint ring (CCW), before the union's weld inflation
  /// — exactly the polygon whose overlaps the sort index arbitrates. The tool
  /// draws it and hit-tests the cursor against it.
  std::vector<std::array<double, 2>> footprint;

  /// The exact border-ring samples with elevation: the Dirichlet sources and
  /// the road mesh's own vertices the floor stitches to.
  std::vector<std::array<double, 3>> border;

  /// Reference-line samples with elevation — the soft interior constraints.
  std::vector<std::array<double, 3>> centerline;
};

/// Every surface span of `junction_id`, in the junction's connection order,
/// de-duplicated by connecting road — the SAME inputs, in the same order, that
/// build_junction_surface flattens into the floor union.
///
/// Returns empty for a stale id, and naturally for a junction with no floor to
/// control: a SPAN (virtual) junction has no connections at all (arms-xor-spans),
/// and a junction whose connecting roads were all erased has nothing to gather.
/// Roads that are stale or carry no geometry are skipped, exactly as the mesher
/// skips them.
///
/// `sampling` must match what the mesh build uses, or the returned samples will
/// not be the ones the floor was built from.
[[nodiscard]] RM_API std::vector<JunctionSurfaceSpanInfo> junction_surface_spans(
    const RoadNetwork& network, JunctionId junction_id, const SamplingOptions& sampling = {});

} // namespace roadmaker
