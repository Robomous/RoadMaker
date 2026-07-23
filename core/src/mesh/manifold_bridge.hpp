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

// The Manifold adapter layer (p5-s3, #233) — Manifold 3.5.2's FIRST consumer in
// the repo. It was pinned and linked (cmake/deps.cmake, THIRD_PARTY_LICENSES.md)
// for exactly this: robust solid booleans on generated bridge geometry. This
// header is the whole integration surface the bridge generator (bridge_solids.cpp)
// sits on, so the Manifold API touches exactly one place.
//
// Design decision recorded here and as an As-built note in
// docs/design/materials-structures/02_bridge_generator.md §3:
//   * The deck is a hand-parametrised sweep built on Manifold::Extrude (which
//     guarantees a closed, correctly-wound solid) followed by Warp (which moves
//     vertices but preserves topology). A curved/superelevated deck is therefore
//     watertight BY CONSTRUCTION with no hand-rolled winding — Extrude does the
//     topology, Warp bends it along the reference line. This is why
//     MANIFOLD_CROSS_SECTION stays OFF (cmake/deps.cmake): we never need
//     Manifold's own 2D cross-section sweep.
//   * MANIFOLD_PAR stays OFF: a deck-sized boolean is sub-millisecond.

#include "roadmaker/mesh/mesh.hpp" // SubMesh

#include <manifold/manifold.h>

#include <array>
#include <cstddef>
#include <functional>
#include <vector>

namespace roadmaker::bridge {

/// One point of a swept cross-section, in the section's local plane: `t` is the
/// lateral axis (across the deck, +left of the reference line) and `w` the
/// vertical axis (+up). The sweep maps every (t, w) to a world point through the
/// caller's frame, so the same section serves a straight test deck and a
/// curved, superelevated, widening road deck.
struct SectionPoint {
  double t = 0.0;
  double w = 0.0;
};

/// Maps a cross-section point at longitudinal fraction `u` in [0, 1] along the
/// span to a world (x, y, z). Supplied by the caller: the generator plugs in the
/// road's station frame (plan view + elevation + superelevation); the spike
/// plugs in a straight ramp.
using SweepFrameFn = std::function<std::array<double, 3>(double t, double w, double u)>;

/// Sweeps a closed CCW cross-section (in the t–w plane) along `n_div` intervals
/// — `n_div + 1` station rings — warping every station through `frame`. Returns
/// a closed solid; the caller MUST check `Status() == Manifold::Error::NoError`
/// before use. Built on Extrude (manifold topology) + Warp (vertex move only),
/// so the result is watertight with no hand-rolled winding. `n_div` must be >= 1.
[[nodiscard]] manifold::Manifold
sweep_section(const std::vector<SectionPoint>& section, int n_div, const SweepFrameFn& frame);

/// An axis-aligned box of `size` (x, y, z extents) centred at world `center`.
/// A `Manifold::Cube` translated — used for piers and abutments in Phase 4.
[[nodiscard]] manifold::Manifold box(std::array<double, 3> center, std::array<double, 3> size);

/// Manifold -> `SubMesh` with FLAT (per-triangle) normals and planar UVs in
/// meters (u = world x, v = world y) so 01's materials tile across the solid.
/// Faceted: every triangle gets three unique vertices, which keeps the boxy
/// hard edges of a bridge crisp and matches how the exporters treat a solid.
/// Empty `SubMesh` when the solid has no triangles. `material` is left `None` —
/// the caller assigns the bridge material.
[[nodiscard]] SubMesh to_submesh(const manifold::Manifold& solid);

} // namespace roadmaker::bridge
