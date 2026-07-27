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
// The kernel refuses to transform a junction's CONNECTING road — by design, and
// something the user must be told about BEFORE the edit rather than after. Two
// surfaces need that: SelectTool's move-drag and the transform gizmo. Keeping
// the wording here is what stops the two from drifting apart, which is exactly
// how the gizmo ended up silent while Select was not.
//
// This gate has narrowed twice, and both times because the kernel grew a
// capability the editor was still guarding against:
//
//  - "would this transform sever a link?" went with cascade-s1 (#461). A move
//    takes its linked neighbours with it, so there is no break to warn about,
//    and the rare sever that IS unavoidable cannot be predicted at the grab.
//    Document reports those after the fact (Document::links_severed).
//  - "does this road touch a junction?" went with cascade-s2 (#462). A junction
//    ARM moves now, and the junction regenerates from its new pose — refusing it
//    here would restore the blanket refusal that sprint removed. Only a
//    connecting road, whose pose is generated rather than authored, is left.
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
/// A junction's CONNECTING road has a generated pose — it is planned from the
/// arm poses and the next regeneration overwrites whatever you drag it to — so
/// the kernel refuses to move or rotate it (core/src/edit/operations.cpp,
/// plan_carried_junctions). Mirroring that check here lets the editor refuse at
/// the grab, with a sentence naming the road and the junction, instead of
/// previewing a command that was never going to apply.
///
/// An ARM is deliberately NOT gated: moving one is supported and its junction
/// follows. A move that leaves a junction unbuildable is still refused, but only
/// the kernel can know that — it depends on where the drag ends up, not on what
/// was grabbed — so it arrives as a failed command rather than through here.
///
/// Names the FIRST offending road, matching the kernel's own behaviour.
[[nodiscard]] std::optional<QString> junction_transform_refusal(const RoadNetwork& network,
                                                                std::span<const RoadId> roads,
                                                                TransformKind kind);

} // namespace roadmaker::editor
