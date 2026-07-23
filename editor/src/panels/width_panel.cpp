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

#include "panels/width_panel.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {
constexpr double kMarginLeft = 44.0;
constexpr double kMarginRight = 12.0;
constexpr double kMarginTop = 30.0;
constexpr double kMarginBottom = 24.0;
constexpr double kHitRadiusPx = 9.0;
} // namespace

WidthPanel::WidthPanel(Document& document, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection) {
  setFocusPolicy(Qt::ClickFocus); // Backspace deletes the selected node
  setMouseTracking(false);
  setMinimumHeight(180);
  setToolTip(tr("Lane width w(s) across the selected lane's section.\n"
                "Drag a node vertically to change its width; double-click the curve "
                "inserts a node; Backspace deletes the selected one.\n"
                "Shift+double-click splits the lane section at the cursor.\n\n"
                "Widths are piecewise-linear between nodes; zero width is legal "
                "(a turn lane tapers up from nothing)."));

  auto* controls = new QHBoxLayout;
  controls->setContentsMargins(6, 4, 6, 0);
  header_ = new QLabel(this);
  controls->addWidget(header_);
  controls->addStretch(1);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addLayout(controls);
  layout->addStretch(1); // the plot paints on the panel itself below the row

  connect(
      &selection_, &SelectionModel::selection_changed, this, [this] { refresh_from_document(); });
  connect(&document_, &Document::loaded, this, [this] { refresh_from_document(); });
  connect(&document_, &Document::mesh_changed, this, [this](const std::vector<RoadId>&) {
    if (!drag_active_) {
      refresh_from_document();
    }
  });
  refresh_from_document();
}

std::vector<Poly3> WidthPanel::to_records(const std::vector<WidthPoint>& points) {
  std::vector<Poly3> records;
  records.reserve(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double width = std::max(0.0, points[i].width);
    double slope = 0.0;
    if (i + 1 < points.size()) {
      const double ds = points[i + 1].s_offset - points[i].s_offset;
      if (ds > 1e-9) {
        slope = (std::max(0.0, points[i + 1].width) - width) / ds;
      }
    }
    records.push_back(Poly3{.s = points[i].s_offset, .a = width, .b = slope, .c = 0.0, .d = 0.0});
  }
  return records;
}

void WidthPanel::refresh_from_document() {
  const Lane* lane = document_.network().lane(selection_.primary().lane);
  // The center lane carries no width by rule; nothing to edit there.
  const bool editable = lane != nullptr && lane->odr_id != 0;
  if (!editable) {
    lane_ = LaneId{};
    road_ = RoadId{};
    section_ = LaneSectionId{};
    section_s0_ = 0.0;
    section_length_ = 0.0;
    nodes_.clear();
    selected_node_.reset();
  } else {
    lane_ = selection_.primary().lane;
    section_ = lane->section;
    const LaneSection* section = document_.network().lane_section(section_);
    road_ = section != nullptr ? section->road : RoadId{};
    section_s0_ = section != nullptr ? section->s0 : 0.0;
    const auto end = section_end(document_.network(), section_);
    section_length_ = (end.has_value() ? *end : section_s0_) - section_s0_;
    nodes_.clear();
    for (const Poly3& record : lane->widths) {
      nodes_.push_back(WidthPoint{.s_offset = record.s, .width = record.a});
    }
    std::ranges::sort(nodes_, {}, &WidthPoint::s_offset);
    if (selected_node_.has_value() && *selected_node_ >= nodes_.size()) {
      selected_node_.reset();
    }
  }
  w_max_ = 1.0;
  for (const WidthPoint& node : nodes_) {
    w_max_ = std::max(w_max_, node.width + 0.5);
  }
  if (header_ != nullptr) {
    if (lane_.is_valid()) {
      header_->setText(tr("Lane %1 — section [%2, %3) m")
                           .arg(lane->odr_id)
                           .arg(section_s0_, 0, 'f', 1)
                           .arg(section_s0_ + section_length_, 0, 'f', 1));
    } else {
      header_->setText(tr("Select a lane"));
    }
  }
  update();
}

// --- editing entry points -----------------------------------------------------

void WidthPanel::push_profile(std::vector<WidthPoint> points) {
  if (!lane_.is_valid()) {
    return;
  }
  if (!document_
           .push_command(
               edit::set_lane_width_profile(document_.network(), lane_, to_records(points)))
           .has_value()) {
    return; // surfaced in Diagnostics by push_command
  }
  refresh_from_document();
}

void WidthPanel::drag_node(std::size_t index, double dwidth) {
  if (!lane_.is_valid() || index >= nodes_.size()) {
    return;
  }
  if (!drag_active_) {
    drag_base_ = nodes_;
    drag_active_ = true;
  }
  std::vector<WidthPoint> points = drag_base_;
  points[index].width = std::max(0.0, points[index].width + dwidth);
  const std::vector<Poly3> records = to_records(points);
  const auto factory = [this, records](const RoadNetwork& base) {
    return edit::set_lane_width_profile(base, lane_, records);
  };
  const bool ok =
      document_.preview_active()
          ? document_.update_preview(factory).has_value()
          : document_
                .begin_preview(edit::set_lane_width_profile(document_.network(), lane_, records))
                .has_value();
  if (ok) {
    nodes_ = points;
    update();
  }
}

void WidthPanel::commit_drag() {
  drag_active_ = false;
  drag_base_.clear();
  document_.commit_preview(); // no-op when the press never became a drag
  refresh_from_document();
}

void WidthPanel::cancel_drag() {
  drag_active_ = false;
  drag_base_.clear();
  document_.cancel_preview();
  refresh_from_document();
}

void WidthPanel::insert_node(double s_offset) {
  if (!lane_.is_valid()) {
    return;
  }
  const Lane* lane = document_.network().lane(lane_);
  if (lane == nullptr) {
    return;
  }
  const double clamped = std::clamp(s_offset, 0.0, section_length_);
  // A record must start strictly inside the section and not collide with an
  // existing one (set_lane_width_profile requires strictly ascending sOffsets).
  if (clamped <= 1e-6 || clamped >= section_length_ - 1e-6) {
    return;
  }
  for (const WidthPoint& node : nodes_) {
    if (std::abs(node.s_offset - clamped) < 1e-6) {
      return;
    }
  }
  std::vector<WidthPoint> points = nodes_;
  points.push_back(
      WidthPoint{.s_offset = clamped, .width = std::max(0.0, eval_profile(lane->widths, clamped))});
  std::ranges::sort(points, {}, &WidthPoint::s_offset);
  push_profile(std::move(points));
}

void WidthPanel::remove_node(std::size_t index) {
  if (!lane_.is_valid() || index >= nodes_.size()) {
    return;
  }
  // The kernel requires a width record at sOffset 0; nodes_ is ascending, so
  // index 0 is that record. Never remove it.
  if (index == 0 || nodes_[index].s_offset <= 1e-9) {
    return;
  }
  std::vector<WidthPoint> points = nodes_;
  points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
  selected_node_.reset();
  push_profile(std::move(points));
}

void WidthPanel::split_at(double s_offset) {
  if (!lane_.is_valid() || !road_.is_valid()) {
    return;
  }
  const double station = section_s0_ + std::clamp(s_offset, 0.0, section_length_);
  if (!document_.push_command(edit::split_lane_section(document_.network(), road_, station))
           .has_value()) {
    return; // outside-the-road or other refusal surfaced in Diagnostics
  }
  // The selected lane id survives (the original section keeps [s0, s)); re-read
  // the (now shorter) section and its nodes.
  refresh_from_document();
}

// --- painting and mouse -------------------------------------------------------

double WidthPanel::s_to_x(double s_offset) const {
  const double plot = std::max(1.0, width() - kMarginLeft - kMarginRight);
  return kMarginLeft + (section_length_ > 0.0 ? (s_offset / section_length_) * plot : 0.0);
}

double WidthPanel::w_to_y(double width_m) const {
  const double plot = std::max(1.0, height() - kMarginTop - kMarginBottom);
  return kMarginTop + (1.0 - width_m / std::max(1e-9, w_max_)) * plot;
}

double WidthPanel::x_to_s(double x) const {
  const double plot = std::max(1.0, width() - kMarginLeft - kMarginRight);
  return std::clamp((x - kMarginLeft) / plot, 0.0, 1.0) * section_length_;
}

double WidthPanel::y_to_w(double y) const {
  const double plot = std::max(1.0, height() - kMarginTop - kMarginBottom);
  return std::max(0.0, (1.0 - (y - kMarginTop) / plot) * w_max_);
}

std::optional<std::size_t> WidthPanel::hit_test(const QPointF& pos) const {
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const QPointF node(s_to_x(nodes_[i].s_offset), w_to_y(nodes_[i].width));
    if (QLineF(pos, node).length() <= kHitRadiusPx) {
      return i;
    }
  }
  return std::nullopt;
}

void WidthPanel::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QPalette& palette = this->palette();

  if (!lane_.is_valid()) {
    painter.setPen(palette.color(QPalette::PlaceholderText));
    painter.drawText(rect(), Qt::AlignCenter, tr("Select a lane to edit its width"));
    return;
  }
  const Lane* lane = document_.network().lane(lane_);
  if (lane == nullptr) {
    return;
  }

  // Zero line + frame.
  painter.setPen(QPen(palette.color(QPalette::Mid), 1, Qt::DashLine));
  painter.drawLine(QPointF(kMarginLeft, w_to_y(0.0)), QPointF(width() - kMarginRight, w_to_y(0.0)));
  painter.setPen(palette.color(QPalette::Mid));
  painter.drawText(QPointF(4, w_to_y(0.0) + 4), QStringLiteral("0 m"));

  // The curve, sampled from the actual kernel width profile.
  QPainterPath path;
  const int samples = 240;
  for (int i = 0; i <= samples; ++i) {
    const double s = section_length_ * static_cast<double>(i) / samples;
    const double w = eval_profile(lane->widths, s);
    const QPointF point(s_to_x(s), w_to_y(w));
    if (i == 0) {
      path.moveTo(point);
    } else {
      path.lineTo(point);
    }
  }
  painter.setPen(QPen(palette.color(QPalette::Highlight), 2));
  painter.drawPath(path);

  // Nodes — the same knob language as the profile panel: themed circles, the
  // accent (QPalette::Highlight) marking the selected/grabbed node.
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const QPointF node(s_to_x(nodes_[i].s_offset), w_to_y(nodes_[i].width));
    const bool selected = selected_node_.has_value() && *selected_node_ == i;
    painter.setBrush(selected ? palette.color(QPalette::Highlight) : palette.color(QPalette::Base));
    painter.setPen(
        QPen(selected ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Mid),
             selected ? 2.0 : 1.5));
    painter.drawEllipse(node, selected ? 6.0 : 4.0, selected ? 6.0 : 4.0);
    painter.setPen(palette.color(QPalette::Mid));
    painter.drawText(node + QPointF(8, -8), tr("%1 m").arg(nodes_[i].width, 0, 'f', 2));
  }
}

void WidthPanel::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !lane_.is_valid()) {
    QWidget::mousePressEvent(event);
    return;
  }
  pressed_ = hit_test(event->position());
  press_pos_ = event->position();
  if (pressed_.has_value()) {
    selected_node_ = *pressed_;
    update();
  }
}

void WidthPanel::mouseMoveEvent(QMouseEvent* event) {
  if (!pressed_.has_value()) {
    QWidget::mouseMoveEvent(event);
    return;
  }
  const double dw = y_to_w(event->position().y()) - y_to_w(press_pos_.y());
  drag_node(*pressed_, dw);
}

void WidthPanel::mouseReleaseEvent(QMouseEvent* event) {
  if (pressed_.has_value()) {
    commit_drag();
    pressed_.reset();
  }
  QWidget::mouseReleaseEvent(event);
}

void WidthPanel::mouseDoubleClickEvent(QMouseEvent* event) {
  if (!lane_.is_valid() || event->button() != Qt::LeftButton) {
    return;
  }
  const double s = x_to_s(event->position().x());
  if ((event->modifiers() & Qt::ShiftModifier) != 0) {
    split_at(s); // Shift+double-click cuts the lane section at the cursor
  } else {
    insert_node(s);
  }
}

void WidthPanel::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && drag_active_) {
    cancel_drag();
    pressed_.reset();
    return;
  }
  if ((event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Delete) &&
      selected_node_.has_value()) {
    remove_node(*selected_node_);
    return;
  }
  QWidget::keyPressEvent(event);
}

} // namespace roadmaker::editor
