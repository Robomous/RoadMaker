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

#include "roadmaker/road/id.hpp"

#include <string>
#include <vector>

namespace roadmaker {

/// Where a Surface's boundary comes from. Derived = auto-formed from the roads
/// that enclose an area (P2); Authored = an explicit node graph the user
/// reshaped (P5, p5-s1). A derived surface DETACHES to Authored the moment its
/// boundary is edited, and `revert_surface_to_derived` puts it back — the same
/// derived/locked split the junctions use.
enum class BoundarySource { Derived, Authored };

/// One node of an authored surface boundary: a plan-view position plus the two
/// cubic-Hermite tangents meeting there. The boundary is a CLOSED loop, so
/// every node has both an arriving (`tangent_in`) and a leaving (`tangent_out`)
/// tangent; equal values make the node smooth, different ones make it a corner.
///
/// Tangents are world-meter vectors, NOT unit directions: their length is the
/// Hermite segment scale, so it controls how far the curve bulges. Plan view
/// only — z stays with the elevation solve, which pins the boundary to the
/// surrounding road borders (build_surface_mesh).
struct SurfaceNode {
  double x = 0.0;
  double y = 0.0;
  double tangent_in_x = 0.0;
  double tangent_in_y = 0.0;
  double tangent_out_x = 0.0;
  double tangent_out_y = 0.0;

  friend constexpr bool operator==(const SurfaceNode&, const SurfaceNode&) = default;
};

/// A ground surface filling an area enclosed by roads (#215, GW-2 step 5).
/// A DERIVED surface's boundary is `bounding_roads`: the ordered ring of roads
/// whose inner edges enclose the region, recomputed by derive_surfaces. An
/// AUTHORED surface's boundary is `nodes` instead; `bounding_roads` is kept as
/// PROVENANCE — the ring it detached from still supplies the elevation
/// (Dirichlet) samples, and still claims that face so derive_surfaces does not
/// re-derive a duplicate on top of it.
struct Surface {
  BoundarySource source = BoundarySource::Derived;
  std::vector<RoadId> bounding_roads; ///< ordered ring; deterministic

  /// The authored boundary loop, in ring order. ALWAYS empty on a Derived
  /// surface, and never shorter than 3 on an Authored one (the command layer
  /// enforces both).
  std::vector<SurfaceNode> nodes;

  /// Ground material name ("" = default grass; e.g. "asphalt", "concrete").
  /// A derived surface can never carry a lane-level OpenDRIVE <material>, so
  /// this is its permanent home (p6-s2). Round-trips as a `material` attribute
  /// on the surface's `<userData code="rm:surface">`, written only when set.
  std::string material;

  friend bool operator==(const Surface&, const Surface&) = default;
};

} // namespace roadmaker
