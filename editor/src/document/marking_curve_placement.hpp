/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Marking-curve placement helper (p3-s4, issue #223) for the Marking Curve tool
// (GW-5 step 6). Pure geometry over the kernel's authoring: fits a G1 clothoid
// through the clicked world points, samples it at ~0.5 m, projects each sample
// onto the ANCHOR road's reference line to build the road-frame (s, t)
// centreline edit::apply_marking_curve_asset consumes, and enforces the
// single-road constraint (every sample must stay on the anchor). No widgets, so
// the projection + clamping is unit-testable headless. A crosswalk asset paints
// a striped band; a plain marking asset a solid/dashed line.

#include "roadmaker/edit/markings.hpp"
#include "roadmaker/error.hpp"
#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/object.hpp"

#include <array>
#include <optional>
#include <span>
#include <vector>

#include "document/library_manifest.hpp"

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

class MaterialCatalog;

/// A marking-curve asset is either a crosswalk (a striped band) or a plain lane
/// marking (a solid/dashed line). Both feed the Marking Curve tool; anything
/// else is refused before the first point is placed.
[[nodiscard]] bool is_marking_curve_asset(const LibraryItem& item);

/// The edit::MarkingCurveParams a crosswalk OR marking library `item` authors: a
/// striped band from a Kind::Crosswalk (its width/dash/paint), or a plain line
/// from a Kind::Marking (its mark width, solid). The source asset key + category
/// carry into the instance's rm:markingCurve userData.
[[nodiscard]] edit::MarkingCurveParams
marking_curve_params_from_item(const LibraryItem& item, const MaterialCatalog& materials);

/// The nearest road to (x, y) whose reference line passes within `max_t` [m],
/// or nullopt — the first click anchors the curve to this road, and every later
/// sample must project back onto it. Skips empty-geometry roads.
[[nodiscard]] std::optional<RoadId>
anchor_road_at(const RoadNetwork& network, double x, double y, double max_t);

/// A built marking-curve object plus its owning (anchor) road, ready for
/// edit::add_object.
struct MarkingCurveResult {
  RoadId road;
  Object object;
};

/// Builds the marking-curve `Object` from the clicked `world_points` anchored to
/// `anchor`: fits a clothoid, samples it at ~0.5 m, projects each sample onto the
/// anchor's reference line (s clamped to [0, length]), and authors the outline +
/// <markings> + rm:markingCurve userData via edit::apply_marking_curve_asset.
///
/// Errors (InvalidArgument): fewer than two points, a failed clothoid fit, a
/// sample that leaves the anchor road (the single-road constraint), or the
/// kernel's own curvature-too-tight refusal — surfaced by the tool as status
/// text.
[[nodiscard]] Expected<MarkingCurveResult>
build_marking_curve(const RoadNetwork& network,
                    RoadId anchor,
                    std::span<const Waypoint> world_points,
                    const edit::MarkingCurveParams& params);

/// The CLAMPED centreline as world points, for the live preview: the same
/// fit → sample → project → clamp path build_marking_curve runs, mapped back to
/// world through the anchor's reference line so the ghost shows exactly where the
/// band will snap. Empty when the fit fails or fewer than two points are given
/// (the preview then falls back to the raw ghost polyline).
[[nodiscard]] std::vector<std::array<double, 2>> marking_curve_preview_polyline(
    const RoadNetwork& network, RoadId anchor, std::span<const Waypoint> world_points);

} // namespace roadmaker::editor
