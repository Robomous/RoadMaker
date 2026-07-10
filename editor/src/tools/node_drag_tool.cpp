#include "tools/node_drag_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <cmath>

#include "document/document.hpp"

namespace roadmaker::editor {

namespace {

struct NodeHit {
  RoadId road;
  std::size_t index = 0;
  Waypoint position;
  double dist = 0.0;
};

/// Nearest authoring waypoint within `radius` of the cursor. Only roads
/// carrying rm:waypoints participate — deriving them for foreign files is
/// the Edit Nodes tool's scope (issue #10).
std::optional<NodeHit> pick_node(const RoadNetwork& network, Waypoint cursor, double radius) {
  std::optional<NodeHit> best;
  network.for_each_road([&](RoadId id, const Road& road) {
    if (!road.authoring_waypoints) {
      return;
    }
    for (std::size_t i = 0; i < road.authoring_waypoints->size(); ++i) {
      const Waypoint& node = (*road.authoring_waypoints)[i];
      const double dist = std::hypot(cursor.x - node.x, cursor.y - node.y);
      if (dist > radius) {
        continue;
      }
      if (!best || dist < best->dist) {
        best = NodeHit{.road = id, .index = i, .position = node, .dist = dist};
      }
    }
  });
  return best;
}

} // namespace

NodeDragTool::NodeDragTool(Document& document, QObject* parent)
    : Tool(parent), document_(document) {}

void NodeDragTool::deactivate() {
  abort_drag();
}

bool NodeDragTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || drag_.has_value()) {
    return false;
  }
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};
  const auto hit = pick_node(document_.network(), cursor, pick_radius_);
  if (!hit) {
    return false;
  }
  drag_ = DragState{
      .road = hit->road, .index = hit->index, .original = hit->position, .current = hit->position};
  emit preview_changed();
  return true;
}

bool NodeDragTool::mouse_move(const ToolEvent& event) {
  if (!drag_.has_value()) {
    return false;
  }
  const Waypoint cursor{.x = event.world_x, .y = event.world_y};
  edit::SnapOptions options = snap_options_;
  options.exclude_road = drag_->road;
  drag_->snap = edit::snap_point(document_.network(), cursor, options);
  const Waypoint target = drag_->snap ? drag_->snap->position : cursor;

  const RoadId road = drag_->road;
  const std::size_t index = drag_->index;
  const Expected<void> moved =
      document_.preview_active()
          ? document_.update_preview([&](const RoadNetwork& base) {
              return edit::move_waypoint(base, road, index, target);
            })
          : document_.begin_preview(edit::move_waypoint(document_.network(), road, index, target));
  if (moved.has_value()) {
    drag_->current = target;
  }
  // On failure the session (if any) stays at its last good state; the drag
  // keeps running so a later move can recover.
  emit preview_changed();
  return true;
}

bool NodeDragTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!drag_.has_value()) {
    return false;
  }
  // A click that never previewed pushes nothing; commit_preview is a no-op
  // without an active session.
  document_.commit_preview();
  drag_.reset();
  emit preview_changed();
  return true;
}

bool NodeDragTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape || !drag_.has_value()) {
    return false;
  }
  abort_drag();
  return true;
}

void NodeDragTool::abort_drag() {
  if (!drag_.has_value()) {
    return;
  }
  document_.cancel_preview();
  drag_.reset();
  emit preview_changed();
}

PreviewGeometry NodeDragTool::preview() const {
  PreviewGeometry geometry;
  if (!drag_.has_value()) {
    return geometry;
  }
  geometry.point_positions = {drag_->current.x, drag_->current.y, 0.0};
  if (drag_->snap) {
    geometry.point_positions.insert(geometry.point_positions.end(),
                                    {drag_->snap->position.x, drag_->snap->position.y, 0.0});
  }
  geometry.line_positions = {
      drag_->original.x, drag_->original.y, 0.0, drag_->current.x, drag_->current.y, 0.0};
  return geometry;
}

} // namespace roadmaker::editor
