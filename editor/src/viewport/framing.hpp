#pragma once

// Per-kind selection bounds for framing (P1/GW-1 steps 7-9).
//
// Framing used to copy whole roads and props into a temporary NetworkMesh and
// take the bounds of that, which meant a selected signal framed its entire
// road, a lane framed its whole road, and a junction framed nothing at all.
// These functions compute the bounds of exactly what is selected, straight from
// the tessellation the viewport already holds.
//
// Editor-side on purpose: there is no Aabb in core/, and framing is a viewport
// concern the kernel has no reason to learn. The meshes already carry
// everything needed.

#include "roadmaker/mesh/mesh.hpp"

#include <span>

#include "document/selection_model.hpp"
#include "render/scene_builder.hpp"

namespace roadmaker::editor {

/// World bounds of `entries` within `mesh`, per selection kind:
/// road → its whole RoadMesh; lane → only that lane's patch vertices;
/// object/signal → the instance's position grown by its model's dimensions;
/// junction → its floor mesh. A mixed selection returns the union.
///
/// Entries whose ids are not in the mesh contribute nothing; when none of them
/// resolve the result is `!valid()` and the caller decides what to do.
[[nodiscard]] SceneBounds selection_bounds(const NetworkMesh& mesh,
                                           std::span<const SelectionEntry> entries);

} // namespace roadmaker::editor
