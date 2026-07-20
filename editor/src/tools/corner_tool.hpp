#pragma once

// Corner tool (p4-s1, issue #225). Direct manipulation of a junction's fillet
// corners: hover highlights the solved corner curve and its two extent guides,
// a click makes that corner active, and dragging one of its three handles
// authors the corner — the APEX handle sets the (symmetric) radius, each
// TANGENCY handle sets that side's tangent-leg extent independently.
//
// The active corner is tool-local sub-selection, deliberately NOT a
// SelectionModel entry (there is no CornerId, and the EditNodesTool precedent
// keeps node-level sub-selection in the tool). The owning junction IS mirrored
// into SelectionModel so the Properties pane and the scene tree follow, and
// corner_selection_changed() lets the pane bind to the finer state.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/mesh/junction_corners.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"

#include <array>
#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// The tool's sub-selection: one corner of one junction, named the way the
/// kernel names it — the CCW-adjacent arm pair.
struct ActiveCorner {
  JunctionId junction;
  RoadEnd arm_a;
  RoadEnd arm_b;

  friend bool operator==(const ActiveCorner&, const ActiveCorner&) = default;
};

/// Which of the active corner's three handles a gesture is about.
enum class CornerHandle {
  None,
  Apex,    ///< on the bisector — drags the symmetric radius
  ExtentA, ///< tangency on arm_a's edge — drags extent_a
  ExtentB, ///< tangency on arm_b's edge — drags extent_b
};

class CornerTool : public Tool {
  Q_OBJECT

public:
  CornerTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around the handles and (for the fallback hover
  /// resolve) around a corner's apex.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The corner curve (line_positions), the two extent guides
  /// (dashed_line_positions) and — once a corner is active — its apex and
  /// tangency handles.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The active corner, or nullopt. The properties pane binds to this plus
  /// corner_selection_changed().
  [[nodiscard]] std::optional<ActiveCorner> active_corner() const { return active_; }

  /// The active corner solved against the CURRENT network (radius/extents and
  /// their bounds, `*_authored` flags, the curve). nullopt when nothing is
  /// active or the pair is no longer an adjacent corner.
  [[nodiscard]] std::optional<JunctionCornerInfo> active_corner_info() const;

  /// The handle under the cursor, CornerHandle::None when none is.
  [[nodiscard]] CornerHandle hovered_handle() const { return hovered_handle_; }

  [[nodiscard]] bool dragging() const { return press_.has_value(); }

signals:
  /// The active corner changed (selected, re-selected, or cleared). Carries no
  /// payload — listeners pull active_corner()/active_corner_info().
  void corner_selection_changed();

private:
  /// An armed gesture on a handle. `info` is the press-time snapshot: every
  /// drag frame is computed from it, never from the live (previewed) network,
  /// because update_preview's factory runs against the BASE state.
  struct PressState {
    CornerHandle handle = CornerHandle::None;
    JunctionCornerInfo info;
  };

  /// Solves `corner` against the current network. nullopt when the junction or
  /// the pair is gone.
  [[nodiscard]] std::optional<JunctionCornerInfo> solve(const ActiveCorner& corner) const;

  /// The corner under the cursor: the picked junction's nearest corner when the
  /// pick hit a junction floor, else the nearest corner apex within
  /// pick_radius_ across every junction.
  [[nodiscard]] std::optional<ActiveCorner> resolve_corner(const ToolEvent& event) const;

  /// The active corner's handle within pick_radius_ of the cursor.
  [[nodiscard]] CornerHandle
  pick_handle(const JunctionCornerInfo& info, double world_x, double world_y) const;

  /// Radius the apex drag authors, from the press-time snapshot.
  [[nodiscard]] static double
  radius_for(const JunctionCornerInfo& info, double world_x, double world_y);

  /// Extents the tangency drag authors: the dragged side from the cursor, the
  /// other from the snapshot's effective value, so converting a symmetric
  /// corner to per-side legs never makes it jump.
  [[nodiscard]] static std::array<double, 2>
  extents_for(const JunctionCornerInfo& info, CornerHandle handle, double world_x, double world_y);

  /// Runs one drag frame: begins the preview session on the first move and
  /// replaces it on every later one.
  void update_drag(double world_x, double world_y);

  void set_active(std::optional<ActiveCorner> corner);

  /// Drops every trace of a session (a stale JunctionId across a load is a
  /// crash waiting to happen).
  void reset_all();

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;

  std::optional<ActiveCorner> active_;
  std::optional<ActiveCorner> hovered_;
  CornerHandle hovered_handle_ = CornerHandle::None;
  std::optional<PressState> press_;
};

} // namespace roadmaker::editor
