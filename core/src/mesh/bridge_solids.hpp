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

// The bridge-solid generator (p5-s3, #233) — deck + guardrails + piers +
// abutments for one <bridge> span, built with Manifold (manifold_bridge.hpp) and
// unioned into a single watertight solid. Deterministic: same span + same road
// geometry + same parameters ⇒ byte-identical solid, which is why the solids are
// never serialized (docs/design/materials-structures/02_bridge_generator.md).
//
// Curved / superelevated / widening decks are handled for free: every station is
// sampled through the road's own frame, so a width change mid-span is a taper and
// a curve is a sweep, not a discontinuity (design §5). The one refusal here is a
// span too short to build a sensible solid; skew and overlap are validated where
// a bridge is authored (edit::author_bridge), not in the geometry.

#include "roadmaker/mesh/mesh.hpp"         // SubMesh
#include "roadmaker/mesh/mesh_builder.hpp" // BridgeParams

#include <cstddef>
#include <optional>

namespace roadmaker {

class RoadNetwork;
struct Road;
struct Bridge;

/// Number of piers the span-length rule places (design §4): 0 up to
/// `params.pier_free_span`, otherwise `ceil(span / pier_spacing) - 1` so every
/// gap is at most `pier_spacing`. Exposed so the rule is directly testable.
[[nodiscard]] std::size_t bridge_pier_count(double span, const BridgeParams& params);

/// The unioned solid for one `<bridge>` span, or nullopt when the span is too
/// short (< 2× deck depth) or otherwise degenerate to build. Exposed for tests;
/// remesh_bridges (mesh_builder.hpp) is the production entry point.
[[nodiscard]] std::optional<SubMesh> build_bridge_solid(const RoadNetwork& network,
                                                        const Road& road,
                                                        const Bridge& bridge,
                                                        const BridgeParams& params);

} // namespace roadmaker
