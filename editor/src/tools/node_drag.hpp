// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
