// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "roadmaker/export.hpp"

namespace roadmaker {

class RoadNetwork;

/// Enumerates the BOUNDED faces of the road graph and reconciles the surface
/// arena so that, after the call, the set of surfaces exactly matches the set
/// of areas enclosed by roads (#215, GW-2 step 5).
///
/// The road graph's nodes are welded road endpoints (coincident within
/// tol::kWeldPosition, joined by predecessor/successor links, or meeting at a
/// shared junction); its edges are the non-connecting roads. Junction-internal
/// connecting roads (Road::junction valid) are skipped. Each enclosed area
/// becomes one Surface whose `bounding_roads` is the ordered ring of roads
/// tracing it, canonicalized so the output is byte-stable across runs.
///
/// Id-stable: a loop that persists across calls keeps its SurfaceId; new loops
/// get a fresh surface; vanished loops are erased. Idempotent: a second call
/// with no topology change touches the arena not at all.
RM_API void derive_surfaces(RoadNetwork& network);

} // namespace roadmaker
