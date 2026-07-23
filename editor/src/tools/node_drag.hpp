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

// Shared node-drag machinery for the editing tools (Select/Move, Edit
// Nodes): one grabbed authoring waypoint moved through Document's preview
// session with snapping, committed as exactly one command on release
// (docs/design/m2/01_editing_framework.md §3).

#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

struct NodeDragState {
  RoadId road;
  std::size_t index = 0;
  Waypoint original;
  Waypoint current;
  std::optional<edit::SnapResult> snap;
};

/// One drag frame: snaps `cursor` (the dragged road excluded — its own
/// moving endpoint would mask every other candidate) and previews the move
/// through the document session. On a failed fit the session stays at its
/// last good state and the drag keeps running so a later move can recover.
void update_node_drag(Document& document,
                      NodeDragState& drag,
                      edit::SnapOptions options,
                      const Waypoint& cursor);

/// Appends the drag overlay: the engaged snap position (point) and the
/// original→current tether (line).
void append_node_drag_overlay(const NodeDragState& drag, PreviewGeometry& geometry);

} // namespace roadmaker::editor
