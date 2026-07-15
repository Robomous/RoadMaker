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

#include <functional>
#include <optional>
#include <vector>

#include "tools/node_drag.hpp"
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

  /// Confirmation gate for a whole-road move that would break links to roads
  /// outside the moved set: returns true to proceed, false to cancel. Shown
  /// BEFORE the preview session begins (a modal opened mid-drag swallows the
  /// mouse-release). MainWindow injects a QMessageBox with a "don't ask again"
  /// switch; tests inject a stub. Unset ⇒ links break without asking.
  void set_link_break_confirm(std::function<bool()> confirm) {
    confirm_link_break_ = std::move(confirm);
  }

  [[nodiscard]] bool moving() const { return move_.has_value(); }

  [[nodiscard]] bool moving_object() const { return object_move_.has_value(); }

  /// "Move mode" — the dedicated, discoverable Move tool (issue #176). Hovering
  /// a movable entity shows the 4-arrow cursor and a body drag always moves
  /// (never rubber-bands). The plain Select tool (move_mode off) keeps the
  /// drag-to-move as a power path and still bands from empty space.
  void set_move_mode(bool on) { move_mode_ = on; }

  [[nodiscard]] bool move_mode() const { return move_mode_; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Node handles of every selected road (points), plus during a drag the
  /// engaged snap position (point) and the original→current tether (line),
  /// plus the rubber-band rectangle (lines) while one is spanned.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] bool dragging() const { return drag_.has_value(); }

  [[nodiscard]] bool banding() const { return band_current_.has_value(); }

  [[nodiscard]] QString instruction() const override;

signals:
  /// A double-click on a road body inserted a bend node (already committed as
  /// one command) and asks the app to switch to Edit Nodes and grab it — so a
  /// double-click-then-drag bends the road in one motion. Carries the road and
  /// the new node's index.
  void edit_nodes_requested(RoadId road, std::size_t node_index);

private:
  /// LMB held without a node grab: a click, a rubber band from empty space, or
  /// a whole-road body move once the cursor leaves the click tolerance.
  struct PressState {
    Waypoint world;
    bool on_geometry = false;
    std::optional<RoadId> road;     // the road under the press, for auto-select
    std::optional<ObjectId> object; // the prop under the press, for a prop move
    RoadId object_road;             // owning road of that prop (project target)
  };

  /// An in-flight whole-road move: the set being translated together, the press
  /// anchor, the current (possibly snapped) end, and any engaged endpoint snap.
  struct MoveDragState {
    std::vector<RoadId> roads;
    Waypoint press;
    Waypoint current;
    std::optional<edit::SnapResult> snap;
  };

  /// An in-flight prop (object) move: which object and its owning road. Props are
  /// road-relative, so the drag re-projects the cursor onto the owning road and
  /// previews move_object; one command commits on release (one undo step).
  struct ObjectMoveState {
    ObjectId object;
    RoadId road;

    /// The last frame's cursor had left the road, so the prop holds its last
    /// good station. Tracked so the hint is emitted on the transition rather
    /// than on every mouse-move.
    bool off_road = false;
  };

  /// Nearest authoring waypoint of a SELECTED road within pick_radius_
  /// (handles are only visible — hence grabbable — on selected roads).
  [[nodiscard]] std::optional<NodeDragState> pick_selected_node(const Waypoint& cursor) const;

  /// Applies the rubber band [press..current]: selects roads whose mesh AABB
  /// intersects the world-space rectangle.
  void apply_band_selection(const Waypoint& current, Qt::KeyboardModifiers modifiers);

  /// Ends the node drag reverting any live preview (Esc / deactivate path).
  void abort_drag();

  /// Starts a whole-road move once the press crosses the click tolerance: picks
  /// the move set (the selection if the pressed road is in it, else just the
  /// pressed road, auto-selected), refuses junction roads with a toast, and
  /// asks confirm_link_break_ before breaking any link that leaves the set.
  /// Leaves move_ unset (and the selection untouched) when it refuses.
  void begin_move_drag(Qt::KeyboardModifiers modifiers);

  /// One move frame: single-road drags snap the nearest endpoint to other
  /// roads' endpoints; the whole set previews through translate_roads.
  void update_move_drag(const Waypoint& cursor);

  /// Ends the move reverting any live preview (Esc / deactivate path).
  void abort_move();

  /// Starts a prop move once the press crosses the click tolerance: auto-selects
  /// the prop and opens a preview session. `road` is the prop's owning road.
  void begin_object_move(ObjectId object, RoadId road);

  /// One prop-move frame: projects the cursor onto the owning road and previews
  /// move_object at the new (s, t).
  void update_object_move(const Waypoint& cursor);

  /// Ends the prop move reverting any live preview (Esc / deactivate path).
  void abort_object_move();

  /// In move mode, sets the hover cursor to the 4-arrow when `over_movable`,
  /// else the arrow — deduped so plain hovers stay cheap. No-op off move mode.
  void update_move_cursor(bool over_movable);

  /// Deletes every selected road (Delete/Backspace, 02 §7) — one command
  /// each, wrapped in ONE QUndoStack macro when the selection holds more
  /// than one road. False when there is nothing to delete.
  bool delete_selection();

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;
  double click_tolerance_ = 0.5;
  edit::SnapOptions snap_options_{};
  std::function<bool()> confirm_link_break_;
  std::optional<NodeDragState> drag_;
  std::optional<MoveDragState> move_;
  std::optional<ObjectMoveState> object_move_;
  std::optional<PressState> press_;
  std::optional<Waypoint> band_current_;
  bool move_mode_ = false;
  Qt::CursorShape hover_cursor_ = Qt::ArrowCursor;
};

} // namespace roadmaker::editor
