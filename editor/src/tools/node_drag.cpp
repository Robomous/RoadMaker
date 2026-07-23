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

#include "tools/node_drag.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include "document/document.hpp"

namespace roadmaker::editor {

void update_node_drag(Document& document,
                      NodeDragState& drag,
                      edit::SnapOptions options,
                      const Waypoint& cursor) {
  options.exclude_road = drag.road;
  drag.snap = edit::snap_point(document.network(), cursor, options);
  const Waypoint target =
      drag.snap ? Waypoint{.x = drag.snap->position.x, .y = drag.snap->position.y} : cursor;

  const RoadId road = drag.road;
  const std::size_t index = drag.index;
  // Follow the junctions live (#156): the command carries the move AND the
  // regeneration of every junction the arm touches, so the connecting roads
  // track the arm mid-drag instead of snapping into place on release. On a road
  // that touches no junction this is move_waypoint unchanged.
  const auto follow = [road, index, target](const RoadNetwork& base) {
    return edit::move_waypoint_following_junctions(base, road, index, target);
  };
  const Expected<void> moved = document.preview_active()
                                   ? document.update_preview(follow)
                                   : document.begin_preview(follow(document.network()));
  if (moved.has_value()) {
    drag.current = target;
  }
}

void append_node_drag_overlay(const NodeDragState& drag, PreviewGeometry& geometry) {
  // A snap target gets an extra hovered handle at the snap point (the grabbed
  // node itself already renders at the moved/snapped position).
  if (drag.snap) {
    geometry.add_handle(
        drag.snap->position.x, drag.snap->position.y, 0.0, HandleKind::Node, HandleState::Hovered);
  }
  geometry.line_positions.insert(
      geometry.line_positions.end(),
      {drag.original.x, drag.original.y, 0.0, drag.current.x, drag.current.y, 0.0});
}

} // namespace roadmaker::editor
