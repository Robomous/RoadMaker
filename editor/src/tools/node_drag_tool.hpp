#pragma once

// Phase 0 gate prototype (issue #37, docs/design/m2/01_editing_framework.md
// §3): drags one authoring waypoint through Document's preview session —
// press picks the nearest node, moves re-fit the clothoid live, release
// commits exactly one undo-stack entry, Esc cancels byte-identically. The
// Select/Move tool (issue #9) absorbs this controller; it is headless by
// construction (ToolEvent in, commands + PreviewGeometry out).

#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

class NodeDragTool : public Tool {
  Q_OBJECT

public:
  explicit NodeDragTool(Document& document, QObject* parent = nullptr);

  /// Capture radius [m] around a waypoint for the press hit-test.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  /// Snap query options for the drag target; the dragged road is always
  /// excluded (its own moving endpoint would mask every other candidate).
  void set_snap_options(edit::SnapOptions options) { snap_options_ = options; }

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Dragged node (point), original→current tether (line), and the engaged
  /// snap position (second point) when one is active.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] bool dragging() const { return drag_.has_value(); }

private:
  struct DragState {
    RoadId road;
    std::size_t index = 0;
    Waypoint original;
    Waypoint current;
    std::optional<edit::SnapResult> snap;
  };

  /// Ends the drag, reverting any live preview (Esc / deactivate path).
  void abort_drag();

  Document& document_;
  double pick_radius_ = 2.0;
  edit::SnapOptions snap_options_{};
  std::optional<DragState> drag_;
};

} // namespace roadmaker::editor
