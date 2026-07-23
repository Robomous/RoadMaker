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

#include "panels/phase_panel.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"

#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "document/signal_phase_overlay.hpp" // signal_state_color

namespace roadmaker::editor {

namespace {
constexpr double kLabelGutter = 104.0; ///< left column for controller ids
constexpr double kHeaderHeight = 30.0; ///< phase name/duration strip
constexpr double kMarginRight = 10.0;
constexpr double kMarginBottom = 6.0;
constexpr double kBoundaryGrabPx = 5.0;

/// Cell-click cycle order: red → green → yellow → red (Off is only ever entered
/// programmatically via the derived maintenance cycle, so a click leaving it
/// returns to Green).
[[nodiscard]] SignalState cycle_state(SignalState state) {
  switch (state) {
  case SignalState::Red:
    return SignalState::Green;
  case SignalState::Green:
    return SignalState::Yellow;
  case SignalState::Yellow:
    return SignalState::Red;
  case SignalState::Off:
    break;
  }
  return SignalState::Green;
}
} // namespace

PhasePanel::PhasePanel(Document& document, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection) {
  setFocusPolicy(Qt::ClickFocus); // Delete removes the selected phase
  setMinimumHeight(160);
  setToolTip(tr("Signal cycle of the selected junction: one row per controller, "
                "one column per phase.\nClick a cell to cycle its color "
                "(green → yellow → red); drag a phase boundary to change its "
                "duration.\nLeft/Right select phases; Delete removes the selected one; "
                "right-click to add or duplicate.\n\n"
                "Phase timing is RoadMaker Layer 1 (rm:phases) — OpenDRIVE keeps "
                "the cycle itself outside the standard (§14.6)."));

  connect(&selection_, &SelectionModel::selection_changed, this, [this] {
    retarget_from_selection();
    refresh_from_document();
  });
  connect(&document_, &Document::loaded, this, [this] {
    junction_ = JunctionId{};
    selected_ = 0;
    playhead_ = 0.0;
    retarget_from_selection();
    refresh_from_document();
  });
  connect(&document_, &Document::mesh_changed, this, [this](const std::vector<RoadId>&) {
    if (!drag_active_) {
      refresh_from_document();
    }
  });
  retarget_from_selection();
  refresh_from_document();
}

// --- targeting + refresh ------------------------------------------------------

void PhasePanel::retarget_from_selection() {
  const RoadNetwork& network = document_.network();
  JunctionId candidate = selection_.primary().junction;
  if (!candidate.is_valid()) {
    if (const Road* road = network.road(selection_.primary().road); road != nullptr) {
      candidate = road->junction; // a connecting road names the junction it belongs to
    }
  }
  if (!candidate.is_valid() || network.junction(candidate) == nullptr) {
    return; // a head/plain-road/empty selection leaves the target alone
  }
  if (candidate != junction_) {
    junction_ = candidate;
    selected_ = 0;
    playhead_ = 0.0;
  }
}

void PhasePanel::refresh_from_document() {
  const RoadNetwork& network = document_.network();
  if (!junction_.is_valid() || network.junction(junction_) == nullptr) {
    junction_ = JunctionId{};
    plan_ = JunctionPhasePlan{};
    selected_ = 0;
    playhead_ = 0.0;
    emit phase_view_changed();
    update();
    return;
  }
  plan_ = junction_phases(network, junction_);
  if (plan_.phases.empty()) {
    selected_ = 0;
    playhead_ = 0.0;
  } else {
    selected_ = std::clamp(selected_, 0, phase_count() - 1);
    if (plan_.cycle_duration > 0.0) {
      playhead_ = std::fmod(playhead_, plan_.cycle_duration);
      if (playhead_ < 0.0) {
        playhead_ += plan_.cycle_duration;
      }
    } else {
      playhead_ = 0.0;
    }
  }
  emit phase_view_changed();
  update();
}

// --- view state ---------------------------------------------------------------

void PhasePanel::select_phase(int i) {
  if (plan_.phases.empty()) {
    return;
  }
  const int n = phase_count();
  selected_ = ((i % n) + n) % n;
  playhead_ = plan_.phases[static_cast<std::size_t>(selected_)].start;
  emit phase_view_changed();
  update();
}

void PhasePanel::next_phase() {
  select_phase(selected_ + 1);
}

void PhasePanel::prev_phase() {
  select_phase(selected_ - 1);
}

void PhasePanel::scrub_to(double t) {
  if (plan_.phases.empty() || plan_.cycle_duration <= 0.0) {
    return;
  }
  playhead_ = std::fmod(t, plan_.cycle_duration);
  if (playhead_ < 0.0) {
    playhead_ += plan_.cycle_duration;
  }
  const std::size_t idx = phase_index_at(plan_, playhead_);
  if (idx != SIZE_MAX) {
    selected_ = static_cast<int>(idx);
  }
  emit phase_view_changed();
  update();
}

std::size_t PhasePanel::playhead_index() const {
  return plan_.phases.empty() ? SIZE_MAX : phase_index_at(plan_, playhead_);
}

std::vector<PhaseSignalState> PhasePanel::signal_states_at_playhead() const {
  const std::size_t i = playhead_index();
  if (i == SIZE_MAX || i >= plan_.phases.size()) {
    return {};
  }
  return plan_.phases[i].signal_states;
}

std::vector<RoadId> PhasePanel::moving_roads() const {
  const std::size_t i = playhead_index();
  if (i == SIZE_MAX || i >= plan_.phases.size()) {
    return {};
  }
  return plan_.phases[i].moving;
}

// --- edits --------------------------------------------------------------------

namespace {
void push_and_refresh(Document& document,
                      std::unique_ptr<edit::Command> command,
                      const std::function<void()>& refresh) {
  if (command != nullptr && document.push_command(std::move(command)).has_value()) {
    refresh();
  }
}
} // namespace

void PhasePanel::add_phase() {
  if (!junction_.is_valid() || plan_.phases.empty()) {
    return; // a derived/authored cycle exists; nothing to add to on an unsignalized junction
  }
  const std::size_t at = static_cast<std::size_t>(selected_) + 1; // after the selected column
  push_and_refresh(document_, edit::add_signal_phase(document_.network(), junction_, at), [this] {
    refresh_from_document();
  });
}

void PhasePanel::duplicate_phase(int i) {
  if (!junction_.is_valid() || i < 0 || i >= phase_count()) {
    return;
  }
  push_and_refresh(
      document_,
      edit::duplicate_signal_phase(document_.network(), junction_, static_cast<std::size_t>(i)),
      [this] { refresh_from_document(); });
}

void PhasePanel::remove_phase(int i) {
  if (!junction_.is_valid() || i < 0 || i >= phase_count()) {
    return;
  }
  // Removing the last remaining phase would leave a zero-phase authored cycle,
  // which is unrepresentable — route it to the derived cycle instead.
  std::unique_ptr<edit::Command> command =
      phase_count() <= 1
          ? edit::clear_signal_phases(document_.network(), junction_)
          : edit::remove_signal_phase(document_.network(), junction_, static_cast<std::size_t>(i));
  push_and_refresh(document_, std::move(command), [this] { refresh_from_document(); });
}

void PhasePanel::remove_selected_phase() {
  if (!plan_.phases.empty()) {
    remove_phase(selected_);
  }
}

void PhasePanel::set_controller_state(int phase, int row, SignalState state) {
  if (phase < 0 || phase >= phase_count() || row < 0 ||
      row >= static_cast<int>(plan_.controller_odr_ids.size())) {
    return;
  }
  const std::vector<PhaseControllerState>& states =
      plan_.phases[static_cast<std::size_t>(phase)].states;
  if (row < static_cast<int>(states.size()) &&
      states[static_cast<std::size_t>(row)].state == state) {
    return; // unchanged — the kernel rejects a no-op, so never push one
  }
  push_and_refresh(document_,
                   edit::set_phase_state(document_.network(),
                                         junction_,
                                         static_cast<std::size_t>(phase),
                                         plan_.controller_odr_ids[static_cast<std::size_t>(row)],
                                         state),
                   [this] { refresh_from_document(); });
}

// --- boundary drag ------------------------------------------------------------

void PhasePanel::drag_boundary(int boundary, double dt) {
  if (!junction_.is_valid() || boundary < 0 || boundary + 1 >= phase_count()) {
    return;
  }
  if (!drag_active_) {
    drag_base_durations_.clear();
    for (const JunctionPhaseInfo& phase : plan_.phases) {
      drag_base_durations_.push_back(phase.duration);
    }
    drag_active_ = true;
  }
  const double base = drag_base_durations_[static_cast<std::size_t>(boundary)];
  const double new_dur = std::clamp(base + dt, 0.1, kMaxSignalPhaseDuration);
  const auto index = static_cast<std::size_t>(boundary);
  const auto factory = [this, index, new_dur](const RoadNetwork& base_net) {
    return edit::set_phase_duration(base_net, junction_, index, new_dur);
  };
  const bool ok = document_.preview_active()
                      ? document_.update_preview(factory).has_value()
                      : document_
                            .begin_preview(edit::set_phase_duration(
                                document_.network(), junction_, index, new_dur))
                            .has_value();
  if (ok) {
    // Reflow locally so the timeline redraws without re-querying mid-drag.
    plan_.phases[index].duration = new_dur;
    double acc = 0.0;
    for (JunctionPhaseInfo& phase : plan_.phases) {
      phase.start = acc;
      acc += phase.duration;
    }
    plan_.cycle_duration = acc;
    update();
  }
}

void PhasePanel::commit_drag() {
  drag_active_ = false;
  drag_base_durations_.clear();
  document_.commit_preview(); // no-op when no preview ever began (unchanged value)
  refresh_from_document();
}

void PhasePanel::cancel_drag() {
  drag_active_ = false;
  drag_base_durations_.clear();
  document_.cancel_preview();
  refresh_from_document();
}

// --- coordinate mapping -------------------------------------------------------

double PhasePanel::plot_left() const {
  return kLabelGutter;
}

double PhasePanel::plot_width() const {
  return std::max(1.0, width() - kLabelGutter - kMarginRight);
}

double PhasePanel::t_to_x(double t) const {
  const double cycle = plan_.cycle_duration;
  return plot_left() + (cycle > 0.0 ? (t / cycle) : 0.0) * plot_width();
}

double PhasePanel::x_to_t(double x) const {
  const double cycle = plan_.cycle_duration;
  if (cycle <= 0.0) {
    return 0.0;
  }
  return std::clamp((x - plot_left()) / plot_width(), 0.0, 1.0) * cycle;
}

PhasePanel::Hit PhasePanel::hit_test(const QPointF& pos) const {
  if (plan_.phases.empty()) {
    return Hit{};
  }
  // Boundary handles first — they sit on top of the cells they separate.
  for (int b = 0; b + 1 < phase_count(); ++b) {
    const auto idx = static_cast<std::size_t>(b);
    const double bx = t_to_x(plan_.phases[idx].start + plan_.phases[idx].duration);
    if (std::abs(pos.x() - bx) <= kBoundaryGrabPx && pos.y() >= 0.0) {
      return Hit{.kind = HitKind::Boundary, .index = b, .row = -1};
    }
  }
  if (pos.x() < plot_left()) {
    return Hit{};
  }
  const auto phase = static_cast<int>(phase_index_at(plan_, x_to_t(pos.x())));
  if (pos.y() < kHeaderHeight) {
    return Hit{.kind = HitKind::Cell, .index = phase, .row = -1}; // header selects the column
  }
  const int nrows = static_cast<int>(plan_.controller_odr_ids.size());
  const double top = kHeaderHeight;
  const double bottom = height() - kMarginBottom;
  if (nrows <= 0 || bottom <= top) {
    return Hit{.kind = HitKind::Cell, .index = phase, .row = -1};
  }
  const double row_h = (bottom - top) / nrows;
  const int row = static_cast<int>((pos.y() - top) / row_h);
  if (row < 0 || row >= nrows) {
    return Hit{.kind = HitKind::Cell, .index = phase, .row = -1};
  }
  return Hit{.kind = HitKind::Cell, .index = phase, .row = row};
}

// --- painting -----------------------------------------------------------------

void PhasePanel::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing);
  const QPalette& palette = this->palette();

  if (plan_.phases.empty()) {
    painter.setPen(palette.color(QPalette::PlaceholderText));
    painter.drawText(rect(),
                     Qt::AlignCenter,
                     junction_.is_valid() ? tr("Signalize this junction to edit its signal phases")
                                          : tr("Select a signalized junction"));
    return;
  }

  const double top = kHeaderHeight;
  const double bottom = height() - kMarginBottom;
  const int nrows = static_cast<int>(plan_.controller_odr_ids.size());
  const double row_h = nrows > 0 ? (bottom - top) / nrows : (bottom - top);

  // Header strip: one labelled column per phase, the selected one accent-outlined.
  for (int i = 0; i < phase_count(); ++i) {
    const JunctionPhaseInfo& phase = plan_.phases[static_cast<std::size_t>(i)];
    const double x0 = t_to_x(phase.start);
    const double x1 = t_to_x(phase.start + phase.duration);
    const QRectF header(x0, 2.0, x1 - x0, kHeaderHeight - 4.0);
    painter.setBrush(palette.color(QPalette::AlternateBase));
    painter.setPen(QPen(palette.color(QPalette::Mid), 1));
    painter.drawRect(header);
    painter.setPen(palette.color(QPalette::Text));
    const QString label =
        phase.name.empty()
            ? tr("%1 s").arg(phase.duration, 0, 'g', 3)
            : tr("%1\n%2 s").arg(QString::fromStdString(phase.name)).arg(phase.duration, 0, 'g', 3);
    painter.drawText(header.adjusted(3, 0, -3, 0), Qt::AlignVCenter | Qt::AlignLeft, label);
    if (i == selected_) {
      painter.setBrush(Qt::NoBrush);
      painter.setPen(QPen(palette.color(QPalette::Highlight), 2));
      painter.drawRect(header.adjusted(1, 1, -1, -1));
    }
  }

  // Controller rows: a colored cell per phase, the state's traffic color.
  for (int r = 0; r < nrows; ++r) {
    const double y = top + (r * row_h);
    painter.setPen(palette.color(QPalette::Text));
    painter.drawText(QRectF(4.0, y, kLabelGutter - 8.0, row_h),
                     Qt::AlignVCenter | Qt::AlignLeft,
                     QString::fromStdString(plan_.controller_odr_ids[static_cast<std::size_t>(r)]));
    for (int i = 0; i < phase_count(); ++i) {
      const JunctionPhaseInfo& phase = plan_.phases[static_cast<std::size_t>(i)];
      const auto row = static_cast<std::size_t>(r);
      const SignalState state =
          row < phase.states.size() ? phase.states[row].state : SignalState::Red;
      const double x0 = t_to_x(phase.start);
      const double x1 = t_to_x(phase.start + phase.duration);
      const QRectF cell(x0, y, x1 - x0, row_h);
      painter.setBrush(signal_state_color(state));
      painter.setPen(QPen(palette.color(QPalette::Window), 1));
      painter.drawRect(cell);
    }
  }

  // The playhead.
  const double px = t_to_x(playhead_);
  painter.setPen(QPen(palette.color(QPalette::Highlight), 2));
  painter.drawLine(QPointF(px, 0.0), QPointF(px, bottom));
}

// --- mouse + keys -------------------------------------------------------------

void PhasePanel::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || plan_.phases.empty()) {
    QWidget::mousePressEvent(event);
    return;
  }
  const Hit hit = hit_test(event->position());
  press_pos_ = event->position();
  press_is_drag_ = false;
  pressed_boundary_ = -1;
  pressed_cell_.reset();
  if (hit.kind == HitKind::Boundary) {
    pressed_boundary_ = hit.index;
  } else if (hit.kind == HitKind::Cell) {
    select_phase(hit.index);
    pressed_cell_ = hit;
  }
}

void PhasePanel::mouseMoveEvent(QMouseEvent* event) {
  if (pressed_boundary_ < 0) {
    QWidget::mouseMoveEvent(event);
    return;
  }
  if (!press_is_drag_ && std::abs(event->position().x() - press_pos_.x()) > 3.0) {
    press_is_drag_ = true;
  }
  if (press_is_drag_) {
    drag_boundary(pressed_boundary_, x_to_t(event->position().x()) - x_to_t(press_pos_.x()));
  }
}

void PhasePanel::mouseReleaseEvent(QMouseEvent* event) {
  if (pressed_boundary_ >= 0) {
    if (press_is_drag_ || drag_active_) {
      commit_drag();
    }
    pressed_boundary_ = -1;
    press_is_drag_ = false;
    QWidget::mouseReleaseEvent(event);
    return;
  }
  if (pressed_cell_.has_value() && pressed_cell_->kind == HitKind::Cell &&
      pressed_cell_->row >= 0) {
    const int phase = pressed_cell_->index;
    const auto row = static_cast<std::size_t>(pressed_cell_->row);
    if (phase >= 0 && phase < phase_count() && row < plan_.controller_odr_ids.size()) {
      const std::vector<PhaseControllerState>& states =
          plan_.phases[static_cast<std::size_t>(phase)].states;
      const SignalState current = row < states.size() ? states[row].state : SignalState::Red;
      set_controller_state(phase, pressed_cell_->row, cycle_state(current));
    }
  }
  pressed_cell_.reset();
  QWidget::mouseReleaseEvent(event);
}

void PhasePanel::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
  case Qt::Key_Delete:
  case Qt::Key_Backspace:
    remove_selected_phase();
    return;
  case Qt::Key_Left:
    prev_phase();
    return;
  case Qt::Key_Right:
    next_phase();
    return;
  case Qt::Key_Escape:
    if (drag_active_) {
      cancel_drag();
      pressed_boundary_ = -1;
      press_is_drag_ = false;
    }
    return;
  default:
    QWidget::keyPressEvent(event);
  }
}

void PhasePanel::contextMenuEvent(QContextMenuEvent* event) {
  const Hit hit = hit_test(event->pos());
  if (hit.kind == HitKind::Cell && hit.index >= 0) {
    select_phase(hit.index);
  }
  const bool has_phases = !plan_.phases.empty();

  QMenu menu(this);
  QAction* add = menu.addAction(tr("Add Phase"), this, [this] { add_phase(); });
  QAction* dup =
      menu.addAction(tr("Duplicate Phase"), this, [this] { duplicate_phase(selected_); });
  QAction* del = menu.addAction(tr("Delete Phase"), this, [this] { remove_selected_phase(); });
  menu.addSeparator();
  QAction* next = menu.addAction(tr("Next Phase"), this, [this] { next_phase(); });
  QAction* prev = menu.addAction(tr("Previous Phase"), this, [this] { prev_phase(); });
  add->setEnabled(has_phases);
  dup->setEnabled(has_phases);
  del->setEnabled(has_phases);
  next->setEnabled(has_phases);
  prev->setEnabled(has_phases);
  menu.exec(event->globalPos());
}

} // namespace roadmaker::editor
