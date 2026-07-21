#pragma once

// Maneuver tool (p4-s6, issue #227). Direct manipulation of ONE connecting
// road's path through a junction — the "maneuver".
//
// The junction's turns are otherwise a black box: the generator plans them, the
// mesher draws them, and an author who wants one to swing wider has nothing to
// grab. This tool draws every maneuver of the junction under attention, makes
// one of them active, and lets its INTERIOR control points and its two endpoint
// slides be dragged. Every geometry gesture is one preview session and exactly
// ONE edit::set_maneuver_path on release, which locks the maneuver in the same
// undo step (hand-shaped geometry the next regeneration threw away would be a
// data-loss bug).
//
// PICKING WITHOUT A MESH: connecting-road surfaces are deliberately not
// tessellated (mesh_builder.cpp, issue #103), so there is no proxy to ray-cast.
// The tool resolves the junction (from the selection, or from a floor pick) and
// then runs a SCREEN-space min-distance test against each maneuver's sampled
// centerline — constant pixel tolerance at any zoom, via
// editor::screen_distance_to_polyline. Headless callers with no ScreenContext
// fall back to a world-metre radius, which is what the tests drive.
//
// The active maneuver is tool-local sub-selection (there is no ManeuverId), the
// CornerTool / JunctionSurfaceTool precedent. Its connecting ROAD is mirrored
// into SelectionModel — roads are already a selection kind, which is what makes
// a maneuver selectable and highlightable everywhere else in the editor.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/mesh/junction_maneuvers.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/road.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// The tool's sub-selection: one maneuver of one junction, named the way the
/// kernel names it — by its connecting road, which regeneration keeps stable.
struct ActiveManeuver {
  JunctionId junction;
  RoadId road;

  friend bool operator==(const ActiveManeuver&, const ActiveManeuver&) = default;
};

/// Which of the active maneuver's handles a gesture is about.
enum class ManeuverHandle {
  None,
  Start,  ///< the incoming face endpoint — slides along start_slide
  End,    ///< the outgoing face endpoint — slides along end_slide
  Point,  ///< an authored INTERIOR control point — moves freely
  Insert, ///< a segment midpoint: press inserts a control point and drags it
};

/// A handle of the active maneuver: its kind plus, for Point/Insert, the index
/// into `control_points` it edits (an Insert at index i inserts BEFORE i).
struct ManeuverHandleRef {
  ManeuverHandle kind = ManeuverHandle::None;
  std::size_t index = 0;

  friend bool operator==(const ManeuverHandleRef&, const ManeuverHandleRef&) = default;
};

class ManeuverTool : public Tool {
  Q_OBJECT

public:
  ManeuverTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Fallback capture radius [m], used only when the event carries no
  /// ScreenContext (headless). With one, the tolerance is
  /// `screen_pick_radius()` PIXELS.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  /// Capture radius in logical pixels for path and handle hit tests.
  [[nodiscard]] static constexpr double screen_pick_radius() { return 8.0; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Every maneuver of the junction under attention drawn dashed, the active one
  /// solid, plus the active one's handles: Node knobs on the control points,
  /// Midpoint markers where a point would be inserted, and Sample dots at the
  /// two endpoints with their slide segments as guides.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The junction whose maneuvers are on show, or an invalid id.
  [[nodiscard]] JunctionId inspected_junction() const { return inspected_; }

  /// The maneuvers of the inspected junction, solved against the CURRENT
  /// network.
  [[nodiscard]] std::vector<JunctionManeuverInfo> maneuvers() const;

  /// The active maneuver, or nullopt. The properties pane binds to this plus
  /// maneuver_selection_changed().
  [[nodiscard]] std::optional<ActiveManeuver> active_maneuver() const { return active_; }

  /// The active maneuver solved against the current network. nullopt when
  /// nothing is active or its road is no longer a maneuver of the junction.
  [[nodiscard]] std::optional<JunctionManeuverInfo> active_maneuver_info() const;

  /// Makes `road` the active maneuver of the inspected junction — the panel's
  /// row click. A road that is not a maneuver of it clears the sub-selection.
  void select_maneuver(RoadId road);

  [[nodiscard]] bool dragging() const { return press_.has_value(); }

  /// The handle under the cursor (None when none is).
  [[nodiscard]] ManeuverHandleRef hovered_handle() const { return hovered_handle_; }

signals:
  /// The active maneuver changed (selected, re-selected, or cleared). Carries no
  /// payload — listeners pull active_maneuver()/active_maneuver_info().
  void maneuver_selection_changed();

private:
  /// An armed gesture on a handle. `info` is the PRESS-TIME snapshot: every drag
  /// frame is computed from it, never from the live (previewed) network, because
  /// update_preview's factory runs against the BASE state.
  struct PressState {
    ManeuverHandleRef handle;
    JunctionManeuverInfo info;
    /// The control points the release will author — the snapshot's list with the
    /// Insert already applied, so an inserted point and its drag are ONE command.
    std::vector<Waypoint> points;
  };

  /// Re-reads the selection: a junction selection inspects it, a connecting-road
  /// selection inspects that road's junction AND makes it the active maneuver,
  /// anything else clears the tool.
  void sync_to_selection();

  /// The junction the event points at: a picked floor or a picked connecting
  /// road name one outright, otherwise the one already inspected.
  [[nodiscard]] JunctionId resolve_junction(const ToolEvent& event) const;

  /// The maneuver whose path passes nearest the cursor, within tolerance.
  [[nodiscard]] std::optional<RoadId> maneuver_under(const ToolEvent& event) const;

  /// The active maneuver's handle under the cursor.
  [[nodiscard]] ManeuverHandleRef pick_handle(const JunctionManeuverInfo& info,
                                              const ToolEvent& event) const;

  /// Distance from the cursor to `point` in the metric the event supports —
  /// pixels with a ScreenContext, metres without — together with the tolerance
  /// to compare it against.
  [[nodiscard]] std::optional<double> cursor_distance(const ToolEvent& event,
                                                      const std::array<double, 3>& point) const;
  [[nodiscard]] double tolerance(const ToolEvent& event) const;

  /// Runs one drag frame: begins the preview session on the first move and
  /// replaces it on every later one.
  void update_drag(const ToolEvent& event);

  /// The command the current drag frame asks for, given a cursor position.
  [[nodiscard]] std::vector<Waypoint> points_for(double world_x, double world_y) const;
  [[nodiscard]] std::optional<double> start_offset_for(double world_x, double world_y) const;
  [[nodiscard]] std::optional<double> end_offset_for(double world_x, double world_y) const;

  void set_active(std::optional<ActiveManeuver> maneuver);

  /// Drops every trace of a session (a stale JunctionId across a load is a crash
  /// waiting to happen).
  void reset_all();

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;

  JunctionId inspected_;
  std::optional<ActiveManeuver> active_;
  std::optional<RoadId> hovered_;
  ManeuverHandleRef hovered_handle_;
  /// The control point Delete removes — set by the last press or hover on one.
  std::optional<std::size_t> focused_point_;
  std::optional<PressState> press_;
  /// Guards the selection round trip: set_active mirrors the road into
  /// SelectionModel, whose signal calls sync_to_selection right back.
  bool syncing_ = false;
};

} // namespace roadmaker::editor
