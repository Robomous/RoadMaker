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

// Internal (non-installed) companion to the public
// roadmaker/mesh/junction_sidewalk_bands.hpp query: the same band geometry
// reached with a resolved `Junction&` (the mesher already has one) plus the
// Clipper2 form the floor pipeline clips and constrains with.

#include "roadmaker/mesh/junction_sidewalk_bands.hpp"
#include "roadmaker/road/network.hpp"

#include <clipper2/clipper.h>

#include <vector>

namespace roadmaker {

struct Junction;

/// `junction_sidewalk_bands` on an already-resolved junction.
[[nodiscard]] std::vector<JunctionSidewalkBand>
junction_sidewalk_bands_of(const RoadNetwork& network, const Junction& junction);

/// The band's closed plan-view ring: `outer` forward, then `inner` reversed.
/// Winding is not normalized — callers that care run Clipper2 on it.
[[nodiscard]] Clipper2Lib::PathD band_ring(const JunctionSidewalkBand& band);

} // namespace roadmaker
