// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Stencil placement helper (p3-s4, issue #223), shared by the interactive
// Marking Point tool (GW-5 step 6) and the Library drag-drop path so both
// funnel through identical geometry and identical undo semantics. Pure
// translation over the kernel's stencil authoring (edit::apply_stencil_asset) —
// no widgets, no kernel changes — so it is unit-testable headless. Resolves the
// lane under a world cursor to a road-relative pose (s, t clamped to the lane
// band) plus the lane's travel-direction heading, then materializes ONE arrow
// glyph object from the picked Library asset.

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp"

#include <optional>
#include <utility>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

class MaterialCatalog;

/// A stencil drop/click snaps to a lane whose reference line passes within this
/// lateral distance [m] of the cursor — the whole carriageway is in reach so a
/// click anywhere across it grabs the nearest lane, and a click in open space is
/// rejected (OpenDRIVE objects are road-relative). Shared by the tool and the
/// Library drop so the two agree on where a lane is in reach.
inline constexpr double kStencilSnapThreshold = 12.0;

/// A lane pose resolved from a world-space cursor: the owning road, the
/// road-relative station (s, t already clamped to the picked lane's band), the
/// lane's travel-direction heading (odr_id < 0 travels +s → 0; odr_id > 0
/// travels -s → pi; centre lane 0 → 0), and the picked lane's width [m].
struct StencilPose {
  RoadId road;
  double s = 0.0;
  double t = 0.0;
  double hdg = 0.0; ///< [rad] lane travel direction
  double lane_width_m = 0.0;
};

/// The lane pose nearest (x, y): resolves the nearest road within
/// kStencilSnapThreshold, then the lane band containing the cursor t. nullopt
/// when no road is close enough or the cursor is off the carriageway. Used by
/// the tool's hover ghost and the click-to-place path (and the Library drop).
[[nodiscard]] std::optional<StencilPose>
stencil_pose_for_point(const RoadNetwork& network, double x, double y);

/// The lane pose for a cursor projected onto ONE known road — the drag-move
/// path, which keeps the stencil on its owning road (edit::move_object re-locates
/// on the same road). Projects (x, y) onto `road`, clamps t to the containing
/// lane band, and reads the travel heading there. nullopt when the cursor has
/// left the road (farther than kStencilSnapThreshold laterally) or the
/// carriageway.
[[nodiscard]] std::optional<StencilPose>
stencil_pose_on_road(const RoadNetwork& network, RoadId road, double x, double y);

/// The arrow-glyph `Object` a Stencil library `item` places at the lane under
/// (x, y): resolves the pose (stencil_pose_for_point), scales the glyph width to
/// the lane, mints an id_unique_in_class odr_id, and authors the cornerLocal
/// outline via edit::apply_stencil_asset. Returns {owning road, object} ready
/// for edit::add_object (the caller adds it as ONE undo entry). nullopt when the
/// cursor is off any lane or the asset is not a stencil / authors no glyph.
[[nodiscard]] std::optional<std::pair<RoadId, Object>>
stencil_for_point(const RoadNetwork& network,
                  double x,
                  double y,
                  const LibraryItem& item,
                  const MaterialCatalog& materials);

} // namespace roadmaker::editor
