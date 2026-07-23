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

namespace roadmaker {

class RoadNetwork;
struct Junction;

/// Internals of the stop-line solve that a second translation unit needs.
/// Deliberately NOT part of the public kernel API: these are predicates the
/// edit layer must share with `junction_stoplines()` so the two cannot drift,
/// not queries a consumer should reach for (use `junction_stoplines()`).
namespace stopline_detail {

/// True when the stop line keyed by `arm` in `junction` would span at least one
/// driving lane once `flipped` is applied — the precondition `flip_stopline`
/// checks before toggling.
///
/// Lives here because the answer depends on WHICH KIND of key `arm` is: on an
/// arm-based junction the lanes are sampled at the road end `arm` names, while
/// on a span junction `arm` is a pseudo road end and the lanes are sampled at
/// the span edge, in the travel sense that APPROACHES that edge — the opposite
/// contact (p4-s4, issue #319).
[[nodiscard]] bool stopline_direction_has_lanes(const RoadNetwork& network,
                                                const Junction& junction,
                                                const RoadEnd& arm,
                                                bool flipped);

} // namespace stopline_detail
} // namespace roadmaker
