// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "panels/profile_panel.hpp"

#include "roadmaker/geometry/poly3.hpp"
#include "roadmaker/road/network.hpp"

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

#include "document/document.hpp"
#include "document/elevation_utils.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {
constexpr double kMarginLeft = 44.0;
constexpr double kMarginRight = 12.0;
constexpr double kMarginTop = 30.0;
constexpr double kMarginBottom = 24.0;
constexpr double kHitRadiusPx = 9.0;
constexpr double kGradeHandlePx = 34.0; ///< tangent handle arm length on screen
} // namespace

ProfilePanel::ProfilePanel(Document& document, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection) {
  setFocusPolicy(Qt::ClickFocus); // Backspace deletes the selected node
  setMouseTracking(false);
  setMinimumHeight(180);
  setToolTip(tr("Vertical profile z(s) of the selected road.\n"
                "Drag a node vertically to change its elevation; drag a tangent "
                "handle to change its grade.\nDouble-click the curve inserts a "
                "node; Backspace deletes the selected one.\n\n"
                "Edits the elevation profile only — superelevation editing "
                "arrives with M3a."));

  auto* controls = new QHBoxLayout;
  controls->setContentsMargins(6, 4, 6, 0);
  grade_label_ = new QLabel(this);
  controls->addWidget(grade_label_);
  controls->addStretch(1);
  controls->addWidget(new QLabel(tr("Max grade %"), this));
  max_grade_spin_ = new QDoubleSpinBox(this);
  max_grade_spin_->setRange(1.0, 40.0);
  max_grade_spin_->setValue(12.0);
  max_grade_spin_->setDecimals(0);
  connect(max_grade_spin_, &QDoubleSpinBox::valueChanged, this, [this](double) {
    update_grade_label();
  });
  controls->addWidget(max_grade_spin_);
  controls->addWidget(new QLabel(tr("Clearance m"), this));
  clearance_spin_ = new QDoubleSpinBox(this);
  clearance_spin_->setRange(2.0, 20.0);
  clearance_spin_->setValue(5.5);
  clearance_spin_->setDecimals(1);
  controls->addWidget(clearance_spin_);
  auto* over_button = new QPushButton(tr("Cross Over"), this);
  connect(over_button, &QPushButton::clicked, this, [this] { apply_overpass(true); });
  controls->addWidget(over_button);
  auto* under_button = new QPushButton(tr("Cross Under"), this);
  connect(under_button, &QPushButton::clicked, this, [this] { apply_overpass(false); });
  controls->addWidget(under_button);

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

void ProfilePanel::refresh_from_document() {
  const SelectionEntry primary = selection_.primary();
  const Road* road = document_.network().road(primary.road);
  if (road == nullptr) {
    road_ = RoadId{};
    road_length_ = 0.0;
    nodes_.clear();
    selected_node_.reset();
  } else {
    road_ = primary.road;
    road_length_ = road->length;
    nodes_ = edit::elevation_profile_points(*road);
    if (selected_node_.has_value() && *selected_node_ >= nodes_.size()) {
      selected_node_.reset();
    }
  }
  z_min_ = -1.0;
  z_max_ = 1.0;
  for (const edit::ElevationPoint& node : nodes_) {
    z_min_ = std::min(z_min_, node.z - 1.0);
    z_max_ = std::max(z_max_, node.z + 1.0);
  }
  update_grade_label();
  update();
}

double ProfilePanel::max_grade() const {
  const Road* road = document_.network().road(road_);
  if (road == nullptr || road->elevation.empty()) {
    return 0.0;
  }
  double worst = 0.0;
  for (std::size_t i = 0; i < road->elevation.size(); ++i) {
    const Poly3& record = road->elevation[i];
    const double end_s = i + 1 < road->elevation.size() ? road->elevation[i + 1].s : road->length;
    worst = std::max({worst,
                      std::abs(record.eval_derivative(record.s)),
                      std::abs(record.eval_derivative(end_s))});
    if (std::abs(record.d) > 1e-15) {
      const double vertex = record.s - record.c / (3.0 * record.d);
      if (vertex > record.s && vertex < end_s) {
        worst = std::max(worst, std::abs(record.eval_derivative(vertex)));
      }
    }
  }
  return worst;
}

void ProfilePanel::update_grade_label() {
  if (!road_.is_valid()) {
    grade_label_->setText(tr("No road selected"));
    grade_label_->setStyleSheet({});
    return;
  }
  const double worst = max_grade() * 100.0;
  grade_label_->setText(tr("max grade %1 %").arg(worst, 0, 'f', 1));
  const bool too_steep = worst > max_grade_spin_->value();
  // Palette-derived red keeps the warning visible in light and dark themes.
  grade_label_->setStyleSheet(too_steep ? QStringLiteral("color: #c62828; font-weight: bold;")
                                        : QString());
}

double ProfilePanel::clearance() const {
  return clearance_spin_->value();
}

// --- editing entry points -----------------------------------------------------

void ProfilePanel::push_profile(std::vector<edit::ElevationPoint> points) {
  if (!document_
           .push_command(edit::set_elevation_profile(document_.network(), road_, std::move(points)))
           .has_value()) {
    return; // surfaced in Diagnostics by push_command
  }
  refresh_from_document();
}

void ProfilePanel::drag_node(std::size_t index, double dz) {
  if (!road_.is_valid() || index >= nodes_.size()) {
    return;
  }
  if (!drag_active_) {
    drag_base_ = nodes_;
    drag_active_ = true;
  }
  std::vector<edit::ElevationPoint> points = drag_base_;
  points[index].z += dz;
  const auto factory = [this, points](const RoadNetwork& base) {
    return edit::set_elevation_profile(base, road_, points);
  };
  const bool ok =
      document_.preview_active()
          ? document_.update_preview(factory).has_value()
          : document_.begin_preview(edit::set_elevation_profile(document_.network(), road_, points))
                .has_value();
  if (ok) {
    nodes_ = points;
    update_grade_label();
    update();
  }
}

void ProfilePanel::drag_grade(std::size_t index, double grade) {
  if (!road_.is_valid() || index >= nodes_.size()) {
    return;
  }
  if (!drag_active_) {
    drag_base_ = nodes_;
    drag_active_ = true;
  }
  std::vector<edit::ElevationPoint> points = drag_base_;
  points[index].grade = grade;
  const auto factory = [this, points](const RoadNetwork& base) {
    return edit::set_elevation_profile(base, road_, points);
  };
  const bool ok =
      document_.preview_active()
          ? document_.update_preview(factory).has_value()
          : document_.begin_preview(edit::set_elevation_profile(document_.network(), road_, points))
                .has_value();
  if (ok) {
    nodes_ = points;
    update_grade_label();
    update();
  }
}

void ProfilePanel::commit_drag() {
  drag_active_ = false;
  drag_base_.clear();
  document_.commit_preview(); // no-op when the press never became a drag
  refresh_from_document();
}

void ProfilePanel::cancel_drag() {
  drag_active_ = false;
  drag_base_.clear();
  document_.cancel_preview();
  refresh_from_document();
}

void ProfilePanel::insert_node(double s) {
  if (!road_.is_valid()) {
    return;
  }
  const Road* road = document_.network().road(road_);
  std::vector<edit::ElevationPoint> points = nodes_;
  points.push_back(edit::ElevationPoint{
      .s = std::clamp(s, 0.0, road_length_),
      .z = eval_profile(road->elevation, s),
      .grade = std::nullopt,
  });
  push_profile(std::move(points));
}

void ProfilePanel::remove_node(std::size_t index) {
  if (!road_.is_valid() || index >= nodes_.size() || nodes_.size() <= 1) {
    return; // a profile keeps at least one node
  }
  std::vector<edit::ElevationPoint> points = nodes_;
  points.erase(points.begin() + static_cast<std::ptrdiff_t>(index));
  selected_node_.reset();
  push_profile(std::move(points));
}

bool ProfilePanel::apply_overpass(bool over) {
  if (!road_.is_valid()) {
    return false;
  }
  const auto crossings = elevation::find_crossings(document_.network(), road_);
  if (crossings.empty()) {
    grade_label_->setText(tr("No crossing road to pass over/under"));
    return false;
  }
  push_profile(elevation::overpass_points(document_.network(),
                                          road_,
                                          over,
                                          clearance_spin_->value(),
                                          max_grade_spin_->value() / 100.0));
  return true;
}

// --- painting and mouse -------------------------------------------------------

double ProfilePanel::s_to_x(double s) const {
  const double plot = std::max(1.0, width() - kMarginLeft - kMarginRight);
  return kMarginLeft + (road_length_ > 0.0 ? (s / road_length_) * plot : 0.0);
}

double ProfilePanel::z_to_y(double z) const {
  const double plot = std::max(1.0, height() - kMarginTop - kMarginBottom);
  return kMarginTop + (1.0 - (z - z_min_) / std::max(1e-9, z_max_ - z_min_)) * plot;
}

double ProfilePanel::x_to_s(double x) const {
  const double plot = std::max(1.0, width() - kMarginLeft - kMarginRight);
  return std::clamp((x - kMarginLeft) / plot, 0.0, 1.0) * road_length_;
}

double ProfilePanel::y_to_z(double y) const {
  const double plot = std::max(1.0, height() - kMarginTop - kMarginBottom);
  return z_min_ + (1.0 - (y - kMarginTop) / plot) * (z_max_ - z_min_);
}

std::optional<ProfilePanel::Hit> ProfilePanel::hit_test(const QPointF& pos) const {
  // Grade handles first — they sit on top of their node visually.
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const double grade = nodes_[i].grade.value_or(0.0);
    const QPointF node(s_to_x(nodes_[i].s), z_to_y(nodes_[i].z));
    const double angle = std::atan2(-grade, 1.0); // screen y grows downward
    const QPointF tip = node + kGradeHandlePx * QPointF(std::cos(angle), std::sin(angle));
    if (QLineF(pos, tip).length() <= kHitRadiusPx) {
      return Hit{.index = i, .grade_handle = true};
    }
  }
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const QPointF node(s_to_x(nodes_[i].s), z_to_y(nodes_[i].z));
    if (QLineF(pos, node).length() <= kHitRadiusPx) {
      return Hit{.index = i, .grade_handle = false};
    }
  }
  return std::nullopt;
}

void ProfilePanel::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QPalette& palette = this->palette();

  if (!road_.is_valid()) {
    painter.setPen(palette.color(QPalette::PlaceholderText));
    painter.drawText(rect(), Qt::AlignCenter, tr("Select a road to edit its vertical profile"));
    return;
  }
  const Road* road = document_.network().road(road_);
  if (road == nullptr) {
    return;
  }

  // Zero line + frame.
  painter.setPen(QPen(palette.color(QPalette::Mid), 1, Qt::DashLine));
  painter.drawLine(QPointF(kMarginLeft, z_to_y(0.0)), QPointF(width() - kMarginRight, z_to_y(0.0)));
  painter.setPen(palette.color(QPalette::Mid));
  painter.drawText(QPointF(4, z_to_y(0.0) + 4), QStringLiteral("0 m"));

  // The curve, sampled from the actual kernel profile.
  QPainterPath path;
  const int samples = 240;
  for (int i = 0; i <= samples; ++i) {
    const double s = road_length_ * static_cast<double>(i) / samples;
    const double z = eval_profile(road->elevation, s);
    const QPointF point(s_to_x(s), z_to_y(z));
    if (i == 0) {
      path.moveTo(point);
    } else {
      path.lineTo(point);
    }
  }
  painter.setPen(QPen(palette.color(QPalette::Highlight), 2));
  painter.drawPath(path);

  // Nodes + grade handles — the same knob language as the viewport
  // (draw_handles): themed circles, the accent (QPalette::Highlight) marking
  // the selected/grabbed node. QPalette carries the theme tokens
  // (Highlight = accent, Base/Text/Mid), keeping this panel Qt-native.
  for (std::size_t i = 0; i < nodes_.size(); ++i) {
    const QPointF node(s_to_x(nodes_[i].s), z_to_y(nodes_[i].z));
    const double grade = nodes_[i].grade.value_or(0.0);
    const double angle = std::atan2(-grade, 1.0);
    const QPointF arm = kGradeHandlePx * QPointF(std::cos(angle), std::sin(angle));
    const bool selected = selected_node_.has_value() && *selected_node_ == i;

    // Grade arm + its draggable end knob.
    painter.setPen(QPen(palette.color(QPalette::Text), 1));
    painter.drawLine(node - arm, node + arm);
    painter.setBrush(selected ? palette.color(QPalette::Highlight) : palette.color(QPalette::Base));
    painter.setPen(QPen(palette.color(QPalette::Text), 1.5));
    painter.drawEllipse(node + arm, 3.5, 3.5);

    // Node knob: accent fill when selected (grabbed), else a subtle base fill.
    painter.setBrush(selected ? palette.color(QPalette::Highlight) : palette.color(QPalette::Base));
    painter.setPen(
        QPen(selected ? palette.color(QPalette::HighlightedText) : palette.color(QPalette::Mid),
             selected ? 2.0 : 1.5));
    painter.drawEllipse(node, selected ? 6.0 : 4.0, selected ? 6.0 : 4.0);

    painter.setPen(palette.color(QPalette::Mid));
    painter.drawText(node + QPointF(8, -8), tr("%1 %").arg(grade * 100.0, 0, 'f', 1));
  }
}

void ProfilePanel::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !road_.is_valid()) {
    QWidget::mousePressEvent(event);
    return;
  }
  pressed_ = hit_test(event->position());
  press_pos_ = event->position();
  if (pressed_.has_value()) {
    selected_node_ = pressed_->index;
    update();
  }
}

void ProfilePanel::mouseMoveEvent(QMouseEvent* event) {
  if (!pressed_.has_value()) {
    QWidget::mouseMoveEvent(event);
    return;
  }
  if (pressed_->grade_handle) {
    // Handle angle → grade: invert the screen mapping used in hit_test.
    const QPointF node(
        s_to_x(nodes_[pressed_->index].s),
        z_to_y(drag_base_.empty() ? nodes_[pressed_->index].z : drag_base_[pressed_->index].z));
    const QPointF delta = event->position() - node;
    if (std::abs(delta.x()) > 4.0) {
      drag_grade(pressed_->index, -delta.y() / std::abs(delta.x()));
    }
  } else {
    const double dz = y_to_z(event->position().y()) - y_to_z(press_pos_.y());
    drag_node(pressed_->index, dz);
  }
}

void ProfilePanel::mouseReleaseEvent(QMouseEvent* event) {
  if (pressed_.has_value()) {
    commit_drag();
    pressed_.reset();
  }
  QWidget::mouseReleaseEvent(event);
}

void ProfilePanel::mouseDoubleClickEvent(QMouseEvent* event) {
  if (road_.is_valid() && event->button() == Qt::LeftButton) {
    insert_node(x_to_s(event->position().x()));
  }
}

void ProfilePanel::keyPressEvent(QKeyEvent* event) {
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
