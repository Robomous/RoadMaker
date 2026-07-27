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
#include "roadmaker/road/id.hpp"

#include <vector>

namespace roadmaker {

class RoadNetwork;

/// What `derive_surfaces` would change, computed without touching the arena
/// (cascade-s3, #463). Empty on both counts means the surface set already
/// matches the roads — the overwhelmingly common case, and the one a move's
/// derived-layer stage must be able to detect for nothing.
///
/// The split exists because a move has to reconcile the set through an UNDOABLE
/// command: `derive_surfaces` erases a vanished loop's Surface outright, taking
/// its `material` with it, so a plan the command layer can apply itself (with
/// `erase_surface_exact`/`restore_surface`) is what makes undo byte-identical.
struct SurfaceReconciliation {
  /// DERIVED surfaces whose face no longer exists. Never an Authored surface —
  /// its boundary is its own data (p5-s1, decision D3).
  std::vector<SurfaceId> erase;

  /// Enclosed faces with no surface yet, as canonicalized bounding rings, in
  /// the same deterministic order `derive_surfaces` creates them.
  std::vector<std::vector<RoadId>> create;

  [[nodiscard]] bool empty() const { return erase.empty() && create.empty(); }
};

/// The read-only half of `derive_surfaces`: enumerates the bounded faces and
/// diffs them against the arena, without mutating it.
[[nodiscard]] RM_API SurfaceReconciliation plan_surface_reconciliation(const RoadNetwork& network);

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
///
/// Exactly `plan_surface_reconciliation` followed by applying the plan.
RM_API void derive_surfaces(RoadNetwork& network);

} // namespace roadmaker
