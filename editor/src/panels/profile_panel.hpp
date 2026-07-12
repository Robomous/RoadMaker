#pragma once

// Vertical-profile panel (hardening sprint WS-C): a dockable 2D view of the
// selected road's z(s) — elevation nodes draggable in z, grade (tangent)
// handles in %, insert on double-click, delete with Backspace, and the
// overpass workflow ("cross over/under" with a clearance). Every edit is a
// kernel command through Document (drags are ONE preview session committed
// on release); the widget itself stays thin — the interactive entry points
// are public methods the offscreen tests drive directly, and the mouse
// handlers call the same methods.
//
// Honest limits (also in the panel's help tooltip): this edits the road's
// elevation profile only — superelevation editing is M3a+ scope.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/id.hpp"

#include <QWidget>
#include <optional>
#include <vector>

class QDoubleSpinBox;
class QLabel;

namespace roadmaker::editor {

class Document;
class SelectionModel;

class ProfilePanel : public QWidget {
  Q_OBJECT

public:
  ProfilePanel(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  /// The road being edited (invalid when the selection has none).
  [[nodiscard]] RoadId road() const { return road_; }

  [[nodiscard]] const std::vector<edit::ElevationPoint>& nodes() const { return nodes_; }

  // --- interactive entry points (mouse handlers call these; tests too) ------

  /// Starts/updates a z-drag of node `index` by `dz` meters against the
  /// pre-drag profile (ONE preview session; commit_drag() pushes the single
  /// undo entry, cancel_drag() reverts to byte-identical).
  void drag_node(std::size_t index, double dz);

  /// Starts/updates a grade-handle drag of node `index` to `grade` (dz/ds).
  void drag_grade(std::size_t index, double grade);

  void commit_drag();
  void cancel_drag();

  /// Inserts a node at station `s` on the current curve (one undo entry).
  void insert_node(double s);

  /// Deletes node `index`; a profile keeps at least one node.
  void remove_node(std::size_t index);

  /// The overpass workflow: re-profiles the road to cross every crossing
  /// road over (or under) with the panel's clearance (one undo entry).
  /// Returns false when there is nothing to cross or no road is selected.
  bool apply_overpass(bool over);

  [[nodiscard]] double clearance() const;

  /// Worst |dz/ds| of the current profile (for the grade readout).
  [[nodiscard]] double max_grade() const;

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  void refresh_from_document();
  void push_profile(std::vector<edit::ElevationPoint> points);
  void update_grade_label();

  /// Screen mapping for the plot area (margins applied).
  [[nodiscard]] double s_to_x(double s) const;
  [[nodiscard]] double z_to_y(double z) const;
  [[nodiscard]] double x_to_s(double x) const;
  [[nodiscard]] double y_to_z(double y) const;

  struct Hit {
    std::size_t index;
    bool grade_handle; ///< false = the node itself
  };

  [[nodiscard]] std::optional<Hit> hit_test(const QPointF& pos) const;

  Document& document_;
  SelectionModel& selection_;

  RoadId road_;
  double road_length_ = 0.0;
  std::vector<edit::ElevationPoint> nodes_; ///< current (possibly mid-drag)
  double z_min_ = -1.0;
  double z_max_ = 1.0;

  // Drag session state.
  std::vector<edit::ElevationPoint> drag_base_; ///< nodes at drag start
  bool drag_active_ = false;
  std::optional<Hit> pressed_;
  QPointF press_pos_;
  std::optional<std::size_t> selected_node_;

  QDoubleSpinBox* clearance_spin_ = nullptr;
  QDoubleSpinBox* max_grade_spin_ = nullptr;
  QLabel* grade_label_ = nullptr;
  QWidget* canvas_ = nullptr;
};

} // namespace roadmaker::editor
