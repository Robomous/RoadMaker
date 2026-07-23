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

// Internal (non-installed) header: the scene terrain channel is reachable
// through the public build_network_mesh / remesh_terrain API — this pipeline is
// a kernel implementation detail (p5-s2, #232, GW-2 step 7).

#include "roadmaker/geometry/reference_line.hpp"
#include "roadmaker/mesh/mesh.hpp"
#include "roadmaker/road/network.hpp"

namespace roadmaker {

/// Builds the watertight ground surface filling the height field's extent MINUS
/// the road and enclosed-surface footprints, blending each vertex's z between
/// the sampled field height and the nearest road-edge z across a skirt band of
/// width `skirt` meters, then snapping the road-adjacent boundary onto the exact
/// road-mesh border vertices so the seam is watertight.
///
/// Pure and deterministic (fixed iteration order): two calls on the same
/// network produce byte-identical buffers. Returns an empty SubMesh when the
/// network carries no height field or the field is entirely covered by roads.
[[nodiscard]] SubMesh build_terrain_mesh(const RoadNetwork& network,
                                         const SamplingOptions& sampling = {},
                                         double skirt = 8.0);

} // namespace roadmaker
