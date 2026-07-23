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

// Internal (non-installed) header: enclosed-area ground surfaces are reachable
// through the public build_network_mesh / remesh_surfaces API — this pipeline
// is a kernel implementation detail (#215, p2-s7, GW-2 step 5).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

namespace roadmaker {

struct Surface;

/// Builds the watertight ground surface filling the area ENCLOSED by a derived
/// Surface's `bounding_roads` ring.
///
/// Pipeline (mirrors the junction-floor backend in junction_surface.cpp, but
/// keeps the HOLE of the footprint union instead of its interior): Clipper2
/// union of the ring roads' plan-view footprints → select the enclosed region
/// as the largest interior hole (negative-area contour) of that union →
/// constrained Delaunay triangulation with interior Steiner refinement →
/// harmonic (Laplacian) elevation field with Dirichlet boundary taken from the
/// ring roads' inner-edge border elevations → boundary vertices snapped to the
/// road meshes' exact border vertices so the seam is watertight.
///
/// Pure and deterministic (fixed iteration order, no address-dependent
/// containers): two calls on the same network produce byte-identical buffers.
/// Returns an empty SubMesh when the ring encloses no real area (no hole in the
/// footprint union) — the caller drops empty results.
[[nodiscard]] SubMesh build_surface_mesh(const RoadNetwork& network,
                                         const Surface& surface,
                                         const SamplingOptions& sampling = {});

} // namespace roadmaker
