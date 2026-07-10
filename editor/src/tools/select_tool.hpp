#pragma once

// Select/Move tool (issue #9, docs/design/m2/02_editing_tools.md §1) — the
// default tool. Click picks a lane/road (Shift adds, Ctrl/Cmd toggles); an
// LMB drag from empty space spans a rubber-band rectangle selecting the
// roads it touches; an LMB drag on a node handle of a selected road moves
// the waypoint with live clothoid re-fit through Document's preview session
// (absorbs the phase 0 NodeDragTool prototype). Headless by construction:
// ToolEvent in, SelectionModel/commands + PreviewGeometry out.

#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/road.hpp"

#include <cstddef>
#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class SelectTool : public Tool {
  Q_OBJECT

public:
  SelectTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around a node handle for the press hit-test.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  /// Below this extent [m] in both axes a rubber band degenerates to a
  /// click-pick (the viewport scales it to a few screen pixels).
  void set_click_tolerance(double tolerance) { click_tolerance_ = tolerance; }

  /// Snap query options for node drags; the dragged road is always excluded
  /// (its own moving endpoint would mask every other candidate).
  void set_snap_options(edit::SnapOptions options) { snap_options_ = options; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Node handles of every selected road (points), plus during a drag the
  /// engaged snap position (point) and the original→current tether (line),
  /// plus the rubber-band rectangle (lines) while one is spanned.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] bool dragging() const { return drag_.has_value(); }

  [[nodiscard]] bool banding() const { return band_current_.has_value(); }

private:
  struct DragState {
    RoadId road;
    std::size_t index = 0;
    Waypoint original;
    Waypoint current;
    std::optional<edit::SnapResult> snap;
  };

  /// LMB held without a node grab: a click, or a rubber band once the cursor
  /// leaves the click tolerance from an empty-space press.
  struct PressState {
    Waypoint world;
    bool on_geometry = false;
  };

  /// Nearest authoring waypoint of a SELECTED road within pick_radius_
  /// (handles are only visible — hence grabbable — on selected roads).
  [[nodiscard]] std::optional<DragState> pick_selected_node(const Waypoint& cursor) const;

  /// Applies the rubber band [press..current]: selects roads whose mesh AABB
  /// intersects the world-space rectangle.
  void apply_band_selection(const Waypoint& current, Qt::KeyboardModifiers modifiers);

  /// Ends the drag reverting any live preview (Esc / deactivate path).
  void abort_drag();

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;
  double click_tolerance_ = 0.5;
  edit::SnapOptions snap_options_{};
  std::optional<DragState> drag_;
  std::optional<PressState> press_;
  std::optional<Waypoint> band_current_;
};

} // namespace roadmaker::editor
