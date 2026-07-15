#include "panels/scrub_label.hpp"

#include <QKeyEvent>
#include <QMouseEvent>

namespace roadmaker::editor {

ScrubLabel::ScrubLabel(const QString& text, QWidget* parent) : QLabel(text, parent) {
  // The cursor is the whole affordance: a plain label does not look draggable.
  setCursor(Qt::SizeHorCursor);
  // Click-focus so Esc reaches keyPressEvent during a drag (the label is
  // focused by the very press that starts the gesture).
  setFocusPolicy(Qt::ClickFocus);
}

double ScrubLabel::multiplier_for(Qt::KeyboardModifiers modifiers) {
  // ⌘ on macOS and Ctrl elsewhere both arrive as ControlModifier.
  if (modifiers.testFlag(Qt::ControlModifier)) {
    return kCoarseMultiplier;
  }
  if (modifiers.testFlag(Qt::ShiftModifier)) {
    return kFineMultiplier;
  }
  return 1.0;
}

void ScrubLabel::mousePressEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton) {
    QLabel::mousePressEvent(event);
    return;
  }
  pressed_ = true;
  scrubbing_ = false;
  delta_ = 0.0;
  press_pos_ = event->pos();
  last_pos_ = press_pos_;
  event->accept();
}

void ScrubLabel::mouseMoveEvent(QMouseEvent* event) {
  if (!pressed_) {
    QLabel::mouseMoveEvent(event);
    return;
  }
  if (!scrubbing_) {
    if (std::abs(event->pos().x() - press_pos_.x()) <= kDragSlop) {
      event->accept();
      return; // still a click
    }
    scrubbing_ = true;
    emit scrub_started();
  }
  // Scale the motion of THIS step by the modifiers held right now, then
  // accumulate. Scaling the total instead would make the value jump the moment
  // a modifier is pressed or released mid-drag.
  const double step = static_cast<double>(event->pos().x() - last_pos_.x());
  last_pos_ = event->pos();
  delta_ += step * multiplier_for(event->modifiers());
  emit scrub_moved(delta_);
  event->accept();
}

void ScrubLabel::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() != Qt::LeftButton || !pressed_) {
    QLabel::mouseReleaseEvent(event);
    return;
  }
  pressed_ = false;
  if (scrubbing_) {
    scrubbing_ = false;
    emit scrub_finished();
  }
  event->accept();
}

void ScrubLabel::keyPressEvent(QKeyEvent* event) {
  if (event->key() == Qt::Key_Escape && scrubbing_) {
    abort();
    event->accept();
    return;
  }
  QLabel::keyPressEvent(event);
}

void ScrubLabel::abort() {
  if (!scrubbing_) {
    return;
  }
  pressed_ = false;
  scrubbing_ = false;
  delta_ = 0.0;
  emit scrub_cancelled();
}

} // namespace roadmaker::editor
