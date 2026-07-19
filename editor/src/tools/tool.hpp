#pragma once

#include <QObject>
#include <QString>
#include <optional>
#include <vector>

#include "viewport/picking.hpp"
#include "viewport/toast_queue.hpp"

namespace roadmaker::editor {

// M2 editing-tool state machine (skeleton — see docs/m2/01_editing_framework.md
// §4). Tools are viewport-agnostic controllers: they receive abstract events
// (world-space cursor, picks, modifiers) translated by ViewportWidget and act
// on the network exclusively through Document commands, so their interaction
// logic runs headless under gtest.

enum class ToolId {
  Select,
  Move,
  CreateRoad,
  EditNodes,
  LaneProfile,
  Elevation,
  CreateJunction,
  Split,
  Delete,
  LaneAdd,
  LaneForm,
  LaneCarve,
  Crosswalk,
  MarkingPoint,
  MarkingCurve,
  PropPoint,
  PropCurve,
  PropSpan,
  PropPolygon,
};

struct ToolEvent {
  // Cursor ray intersected with the z=0 ground plane, kernel frame (meters).
  double world_x = 0.0;
  double world_y = 0.0;
  std::optional<PickHit> pick;
  // Road-relative station of the cursor, present only when `pick` names a valid
  // road (the cursor is over a road body). The Lane Width page's
  // split-at-cursor consumes it; a click on empty space leaves it unset.
  std::optional<StationCoord> station;
  Qt::MouseButtons buttons = Qt::NoButton;
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

// A draggable/interactive handle knob, positioned in the kernel frame. The
// viewport projects it to screen and paints a DPI-crisp themed sprite whose
// look follows `kind` (node vs. midpoint "insert here" marker) and `state`
// (idle / hovered / grabbed) — so handle size is screen-constant, unlike the
// old world-meter markers that shrank and grew with zoom.
enum class HandleKind {
  Node,     ///< an editable road node (draggable)
  Midpoint, ///< a segment midpoint: click to insert a node
};

enum class HandleState {
  Idle,    ///< resting
  Hovered, ///< under the cursor, or the currently active/selected node
  Grabbed, ///< being dragged
};

struct Handle {
  double x = 0.0;
  double y = 0.0;
  double z = 0.0;
  HandleKind kind = HandleKind::Node;
  HandleState state = HandleState::Idle;
};

// Overlay geometry in the kernel frame. `line_positions` (xyz triples consumed
// pairwise as segments) are the tangent whiskers / drag whiskers / band lines,
// drawn as accent GL lines; `handles` are the node/midpoint knobs, drawn as
// screen-space QPainter sprites.
struct PreviewGeometry {
  std::vector<double> line_positions;
  std::vector<Handle> handles;

  void add_handle(double x,
                  double y,
                  double z = 0.0,
                  HandleKind kind = HandleKind::Node,
                  HandleState state = HandleState::Idle) {
    handles.push_back(Handle{.x = x, .y = y, .z = z, .kind = kind, .state = state});
  }

  [[nodiscard]] bool empty() const { return line_positions.empty() && handles.empty(); }
};

class Tool : public QObject {
  Q_OBJECT

public:
  explicit Tool(QObject* parent = nullptr);
  ~Tool() override;

  // Called by ToolManager on switch; implementations reset their state here.
  virtual void activate() {}

  virtual void deactivate() {}

  // Return true when the event was consumed (the viewport then skips its own
  // handling, e.g. camera navigation on the same button).
  [[nodiscard]] virtual bool mouse_press(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool mouse_move(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool mouse_release(const ToolEvent&) { return false; }

  // Qt double-click sequence is press, release, double-click, release — the
  // pair's first press is delivered normally, so a tool placing points on
  // press sees one point plus this commit gesture (Create Road, 02 §2).
  [[nodiscard]] virtual bool mouse_double_click(const ToolEvent&) { return false; }

  [[nodiscard]] virtual bool key_press(int key, Qt::KeyboardModifiers) {
    return static_cast<void>(key), false;
  }

  [[nodiscard]] virtual PreviewGeometry preview() const { return {}; }

  /// What this tool's click/drag/modifiers do right now — the PERSISTENT
  /// status-bar instruction line (P1/GW-1). It answers "what can I do with
  /// this tool", so it states the interaction, not the shortcut that reached
  /// it (docs/user-guide/shortcuts.md owns keys) and not a transient result
  /// (status_message owns those). Empty = nothing to say.
  [[nodiscard]] virtual QString instruction() const { return {}; }

signals:
  void preview_changed();

  /// Transient, state-dependent guidance or a result ("Merged", a refusal).
  /// Distinct from instruction(): this one comes and goes, that one persists
  /// for as long as the tool is active.
  void status_message(const QString& text);

  /// Requests a viewport cursor shape (first cursor mechanism, kept minimal):
  /// tools emit SizeAllCursor while dragging a whole road, ArrowCursor when the
  /// drag ends. The ViewportWidget forwards it to setCursor.
  void cursor_changed(Qt::CursorShape shape);

  /// A one-shot tool asks the app to switch to another tool — e.g. Split
  /// returns to Select after a successful cut or Esc. MainWindow forwards it to
  /// the ToolManager.
  void request_tool(ToolId id);

  /// A transient result worth a viewport toast (distinct from the status-bar
  /// guidance in status_message) — e.g. "regenerated in place" or a refusal
  /// (finding 5). MainWindow routes it to ViewportWidget::show_toast.
  void toast_requested(const QString& text, ToastSeverity severity);
};

} // namespace roadmaker::editor
