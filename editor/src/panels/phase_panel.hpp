#pragma once

// The Signal Phase Editor timeline (p4-s8, issue #229, GW-4 steps 4-10): a
// QPainter widget that shows a signalized junction's effective cycle as one ROW
// per controller and one COLUMN per phase (widths proportional to duration),
// with a draggable playhead. Modeled on ProfilePanel — every edit is a kernel
// command through Document (a boundary drag is ONE preview session committed on
// release), the widget stays thin, and the interactive entry points are public
// methods the offscreen tests drive directly while the mouse/key handlers call
// the same methods.
//
// The panel is deliberately VIEWPORT-UNAWARE: it exposes the playhead's resolved
// signal states and moving roads as pull getters and emits phase_view_changed;
// MainWindow turns those into the viewport overlay (document/signal_phase_overlay).

#include "roadmaker/mesh/junction_phases.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"

#include <QWidget>
#include <optional>
#include <vector>

namespace roadmaker::editor {

class Document;
class SelectionModel;

class PhasePanel : public QWidget {
  Q_OBJECT

public:
  PhasePanel(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  /// The junction whose cycle is shown (invalid when nothing signalized is
  /// selected).
  [[nodiscard]] JunctionId junction() const { return junction_; }

  /// The effective cycle currently displayed (derived or authored).
  [[nodiscard]] const JunctionPhasePlan& plan() const { return plan_; }

  /// The accent-outlined phase column, or -1 when the plan is empty.
  [[nodiscard]] int selected_phase() const { return plan_.phases.empty() ? -1 : selected_; }

  /// Cycle time [s] of the playhead.
  [[nodiscard]] double playhead() const { return playhead_; }

  // --- view state (NEVER touches undo/network) -------------------------------

  /// Selects phase `i` (wrapped into range) and parks the playhead at its start.
  void select_phase(int i);
  void next_phase();
  void prev_phase();

  /// Moves the playhead to cycle time `t` (wrapped); the selected column follows.
  void scrub_to(double t);

  // --- edits (each pushes exactly ONE command; skips a no-op) ----------------

  /// Inserts an all-red phase after the selected column (at the end when none).
  void add_phase();
  void duplicate_phase(int i);

  /// Removes phase `i`; removing the last remaining phase returns the junction
  /// to its derived cycle (clear_signal_phases) rather than being rejected.
  void remove_phase(int i);
  void remove_selected_phase();

  /// Sets controller `row`'s state in phase `phase`; a no-op (state already
  /// effective) is skipped so the kernel's no-op rejection never surfaces.
  void set_controller_state(int phase, int row, SignalState state);

  // --- boundary drag (preview session + ONE set_phase_duration on release) ---

  /// Drags the boundary between phase `boundary` and `boundary+1` by `dt`
  /// seconds against the pre-drag cycle, changing ONLY phase `boundary`'s
  /// duration. ONE preview session; commit_drag() pushes the single undo entry.
  void drag_boundary(int boundary, double dt);
  void commit_drag();
  void cancel_drag();

  // --- pull getters for the viewport overlay ---------------------------------

  /// The resolved head states of the phase at the playhead (empty when none).
  [[nodiscard]] std::vector<PhaseSignalState> signal_states_at_playhead() const;

  /// The connecting roads that may proceed in the phase at the playhead.
  [[nodiscard]] std::vector<RoadId> moving_roads() const;

signals:
  /// Emitted whenever the playhead, the selection, or the cycle changes — the
  /// seam MainWindow re-reads to rebuild the viewport phase overlay.
  void phase_view_changed();

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void contextMenuEvent(QContextMenuEvent* event) override;

private:
  /// Re-reads the plan for the current junction, clamps the selection/playhead,
  /// and emits phase_view_changed. Guarded against mid-drag re-entrancy.
  void refresh_from_document();

  /// Retargets the junction from the current selection (a junction, or a road
  /// that belongs to one), keeping the current junction on any other selection —
  /// the SignalTool rule. Resets the view only when the junction changes.
  void retarget_from_selection();

  [[nodiscard]] int phase_count() const { return static_cast<int>(plan_.phases.size()); }

  [[nodiscard]] std::size_t playhead_index() const;

  // Screen mapping for the timeline area (label gutter + header strip applied).
  [[nodiscard]] double t_to_x(double t) const;
  [[nodiscard]] double x_to_t(double x) const;
  [[nodiscard]] double plot_left() const;
  [[nodiscard]] double plot_width() const;

  enum class HitKind { None, Boundary, Cell };

  struct Hit {
    HitKind kind = HitKind::None;
    int index = -1; ///< boundary index, or phase column for a Cell
    int row = -1;   ///< controller row for a Cell
  };

  [[nodiscard]] Hit hit_test(const QPointF& pos) const;

  Document& document_;
  SelectionModel& selection_;

  JunctionId junction_;
  JunctionPhasePlan plan_;
  int selected_ = 0;
  double playhead_ = 0.0;

  // Boundary-drag session.
  int pressed_boundary_ = -1;
  QPointF press_pos_;
  bool press_is_drag_ = false;
  std::vector<double> drag_base_durations_;
  bool drag_active_ = false;
  std::optional<Hit> pressed_cell_;
};

} // namespace roadmaker::editor
