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

#include "roadmaker/export.hpp"
#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/surface.hpp"

#include <array>
#include <vector>

namespace roadmaker {

/// Upper bound on the node count a DERIVED surface's seed boundary is
/// simplified to. The union of a ring of road footprints has hundreds of
/// vertices — every station of every bounding road — and handing all of them to
/// the Surface tool would be unusable. The seed is decimated until it fits
/// here, so the first thing the user sees is a graph they can actually grab.
inline constexpr std::size_t kMaxSeedBoundaryNodes = 24;

/// Plan-view step used to tessellate an authored Hermite boundary loop [m].
/// Matches SamplingOptions::max_step so an authored boundary is no coarser than
/// the road borders it stitches to.
inline constexpr double kBoundarySampleStep = 5.0;

/// The editable boundary node graph of `surface_id`.
///
/// - AUTHORED surface: its stored `nodes`, verbatim.
/// - DERIVED surface: a SEED node set computed from the same footprint union
///   the mesher uses (`build_surface_mesh`) — the enclosed region's ring,
///   decimated to at most `kMaxSeedBoundaryNodes` nodes, with Catmull-Rom
///   tangents. Nothing is stored: the seed is what an edit would detach from
///   (decision D3), so the tool can show handles on a surface nobody has
///   touched yet.
///
/// Empty for a stale id, and for a derived surface whose ring encloses no real
/// area (the same guard that yields an empty mesh). `sampling` must match what
/// the mesh build uses, or the seed will not follow the rendered surface.
///
/// Lives at mesh level, not in edit/, because deriving the seed needs the
/// Clipper2 union — the same layering as `junction_surface_spans`.
[[nodiscard]] RM_API std::vector<SurfaceNode> surface_boundary_nodes(
    const RoadNetwork& network, SurfaceId surface_id, const SamplingOptions& sampling = {});

/// Tessellates a closed cubic-Hermite boundary loop into a plan-view polygon,
/// in node order, `step` metres between samples along each segment's chord.
/// Each node contributes its own position followed by the interior samples of
/// the segment leaving it, so the polygon always passes exactly through every
/// node. Fewer than 3 nodes yields an empty polygon.
///
/// The single definition of "what an authored boundary means geometrically":
/// the mesher fills this polygon, the tool draws it, and the command layer
/// validates against it.
[[nodiscard]] RM_API std::vector<std::array<double, 2>>
sample_surface_boundary(const std::vector<SurfaceNode>& nodes, double step = kBoundarySampleStep);

/// True when the tessellated boundary loop crosses itself — the one shape a
/// surface fill cannot resolve. Non-adjacent segment pairs only: consecutive
/// segments legitimately share an endpoint.
[[nodiscard]] RM_API bool surface_boundary_self_intersects(const std::vector<SurfaceNode>& nodes);

} // namespace roadmaker
