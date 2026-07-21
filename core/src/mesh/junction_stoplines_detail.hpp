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
