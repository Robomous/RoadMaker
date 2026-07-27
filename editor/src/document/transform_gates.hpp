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

// The gate every road transform passes before it may start (#401).
//
// The kernel refuses to transform a junction road — by design, and something
// the user must be told about BEFORE the edit rather than after. Two surfaces
// need that: SelectTool's move-drag and the transform gizmo. Keeping the
// wording here is what stops the two from drifting apart, which is exactly how
// the gizmo ended up silent while Select was not.
//
// The second gate this header used to carry — "would this transform sever a
// link?" — is gone with cascade-s1 (#461): a move now takes its linked
// neighbours with it, so there is no longer a break to warn about, and the rare
// sever that IS unavoidable cannot be predicted at the grab. Document reports
// those after the fact instead (Document::links_severed).
//
// No Document, no tool, no widget: a network and a road set in, a message out —
// so both callers and their headless tests can use it.

#include "roadmaker/road/id.hpp"

#include <QString>
#include <optional>
#include <span>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::editor {

/// Which kernel operation the gate is standing in front of. It selects the
/// wording, so the editor never phrases a refusal differently from the kernel.
enum class TransformKind { Translate, Rotate };

/// Why the transform can't happen, or nullopt when it can.
///
/// A road that participates in a junction has a GENERATED pose, so the kernel
/// refuses to move or rotate it (core/src/edit/operations.cpp, translate_roads
/// and rotate_road). Mirroring that check here lets the editor refuse at the
/// grab, with a sentence naming the road and the junction, instead of
/// previewing a command that was never going to apply.
///
/// Names the FIRST offending road, matching the kernel's own behaviour.
[[nodiscard]] std::optional<QString> junction_transform_refusal(const RoadNetwork& network,
                                                                std::span<const RoadId> roads,
                                                                TransformKind kind);

} // namespace roadmaker::editor
