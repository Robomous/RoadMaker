/*
 * Copyright 2026 Robomous
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

// Lane Width panel (p2-s4): a dockable 2D view of the PRIMARY lane's width
// profile w(s_offset) across its owning lane section [s0, section_end). Width
// nodes drag vertically in meters, insert on double-click, delete with
// Backspace; a Shift+double-click splits the lane section at the cursor
// station. Every edit is a kernel command through Document — drags are ONE
// preview session committed on release (edit::set_lane_width_profile), the
// section split is one edit::split_lane_section command. The widget stays thin:
// its interactive entry points are public methods the offscreen tests drive
// directly, and the mouse handlers call the same methods.
//
// The panel authors piecewise-LINEAR width between its nodes (the general form
// authoring a curved cubic is set_lane_width_profile directly). Zero width is
// legal — a turn lane tapers up from nothing.

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/id.hpp"

#include <QWidget>
#include <optional>
#include <vector>

class QLabel;

namespace roadmaker::editor {

class Document;
class SelectionModel;

class WidthPanel : public QWidget {
  Q_OBJECT

public:
  /// One draggable control point: a width [m] at a section-local sOffset [m].
  struct WidthPoint {
    double s_offset = 0.0;
    double width = 0.0;
  };

  WidthPanel(Document& document, SelectionModel& selection, QWidget* parent = nullptr);

  /// The lane being edited (invalid when the selection has none / a center lane).
  [[nodiscard]] LaneId lane() const { return lane_; }

  [[nodiscard]] const std::vector<WidthPoint>& nodes() const { return nodes_; }

  // --- interactive entry points (mouse handlers call these; tests too) ------

  /// Starts/updates a width-drag of node `index` by `dwidth` meters against the
  /// pre-drag profile (ONE preview session; commit_drag() pushes the single
  /// undo entry, cancel_drag() reverts to byte-identical). Width clamps at 0.
  void drag_node(std::size_t index, double dwidth);

  void commit_drag();
  void cancel_drag();

  /// Inserts a node at section-local `s_offset`, its width sampled from the
  /// current profile there (one undo entry).
  void insert_node(double s_offset);

  /// Deletes node `index`; the sOffset-0 record is protected (the kernel
  /// requires a width record at 0). Ascending order is preserved.
  void remove_node(std::size_t index);

  /// Splits the owning lane section at global station `section_s0 + s_offset`
  /// (Shift+double-click). One undo entry; idempotent at an existing boundary.
  /// Re-resolves the selection's lane/section afterwards.
  void split_at(double s_offset);

protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  void refresh_from_document();
  void push_profile(std::vector<WidthPoint> points);

  /// The connected-LINEAR Poly3 records for `points`: record i has a = width_i,
  /// b = (width_{i+1} - width_i)/(s_{i+1} - s_i), c = d = 0; the last record is
  /// flat (b = 0). Poly3::s is the section-local sOffset.
  [[nodiscard]] static std::vector<Poly3> to_records(const std::vector<WidthPoint>& points);

  /// Screen mapping for the plot area (margins applied).
  [[nodiscard]] double s_to_x(double s_offset) const;
  [[nodiscard]] double w_to_y(double width) const;
  [[nodiscard]] double x_to_s(double x) const;
  [[nodiscard]] double y_to_w(double y) const;

  [[nodiscard]] std::optional<std::size_t> hit_test(const QPointF& pos) const;

  Document& document_;
  SelectionModel& selection_;

  LaneId lane_;
  RoadId road_;
  LaneSectionId section_;
  double section_s0_ = 0.0;
  double section_length_ = 0.0;
  std::vector<WidthPoint> nodes_; ///< current (possibly mid-drag), ascending
  double w_max_ = 1.0;

  // Drag session state.
  std::vector<WidthPoint> drag_base_; ///< nodes at drag start
  bool drag_active_ = false;
  std::optional<std::size_t> pressed_;
  QPointF press_pos_;
  std::optional<std::size_t> selected_node_;

  QLabel* header_ = nullptr;
};

} // namespace roadmaker::editor
