#pragma once

// Elevation tool (issue #16, docs/design/m2/02_editing_tools.md §5). With a
// road selected, clicking one of its geometry nodes makes that node the active
// elevation node; the Properties panel then edits its z through
// edit::set_node_elevation (one command per commit), which re-fits the road's
// elevation profile as a smooth C1 cubic. The tool itself never mutates the
// network — it only routes selection and exposes the active node; the overlay
// draws every selected road's node handles plus a highlight on the active one.
// Headless by construction: ToolEvent in, selection + active-node state out.

#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>
#include <utility>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class ElevationTool : public Tool {
  Q_OBJECT

public:
  ElevationTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around node handles (same feel as Edit Nodes).
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Node handles (points) of every selected road plus the active-node
  /// highlight square.
  [[nodiscard]] PreviewGeometry preview() const override;

  /// The node whose elevation the Properties spin box edits: (road, waypoint
  /// index), or nullopt when no node is active.
  [[nodiscard]] std::optional<std::pair<RoadId, std::size_t>> active_node() const {
    return active_;
  }

signals:
  /// Emitted whenever active_node() changes (the Properties panel re-syncs).
  void active_node_changed();

private:
  /// Nearest effective waypoint of a selected road within pick_radius_.
  [[nodiscard]] std::optional<std::pair<RoadId, std::size_t>>
  pick_node(const Waypoint& cursor) const;

  void set_active(std::optional<std::pair<RoadId, std::size_t>> node);

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;
  std::optional<std::pair<RoadId, std::size_t>> active_;
};

} // namespace roadmaker::editor
