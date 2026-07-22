// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "app/tour_overlay.hpp"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QRect>
#include <algorithm>
#include <utility>

namespace roadmaker::editor {

namespace {

constexpr int kCardW = 380;
constexpr int kCardH = 190;
constexpr int kMargin = 20;
constexpr int kRadius = 10;

} // namespace

TourOverlay::TourOverlay(std::vector<TourStep> steps, QWidget* parent)
    : QWidget(parent), controller_(std::move(steps)),
      next_button_(new QPushButton(tr("Next"), this)),
      skip_button_(new QPushButton(tr("Skip tour"), this)) {
  setAttribute(Qt::WA_StyledBackground, false);
  next_button_->setCursor(Qt::PointingHandCursor);
  skip_button_->setCursor(Qt::PointingHandCursor);
  skip_button_->setFlat(true);
  connect(next_button_, &QPushButton::clicked, this, &TourOverlay::advance);
  connect(skip_button_, &QPushButton::clicked, this, &TourOverlay::skip);
}

void TourOverlay::set_target_resolver(std::function<QRect(const QString&)> resolver) {
  resolver_ = std::move(resolver);
}

void TourOverlay::begin() {
  if (parentWidget() != nullptr) {
    setGeometry(parentWidget()->rect());
  }
  controller_.start();
  if (!controller_.active()) {
    emit finished();
    return;
  }
  show();
  raise();
  relayout();
  update();
}

void TourOverlay::resizeEvent(QResizeEvent* /*event*/) {
  relayout();
}

QRect TourOverlay::highlight_rect() const {
  const TourStep* step = controller_.current();
  if (step == nullptr || !resolver_ || step->target.isEmpty()) {
    return {};
  }
  return resolver_(step->target);
}

QRect TourOverlay::card_rect() const {
  const QRect target = highlight_rect();
  if (target.isNull()) {
    // Centre the card.
    return QRect((width() - kCardW) / 2, (height() - kCardH) / 2, kCardW, kCardH);
  }
  // Below the highlighted button, clamped to the widget.
  int x = target.center().x() - (kCardW / 2);
  x = std::max(kMargin, std::min(x, width() - kCardW - kMargin));
  int y = target.bottom() + 16;
  if (y + kCardH > height() - kMargin) {
    y = target.top() - kCardH - 16; // flip above if it would overflow
  }
  y = std::max(kMargin, y);
  return QRect(x, y, kCardW, kCardH);
}

void TourOverlay::relayout() {
  const QRect card = card_rect();
  const int by = card.bottom() - 16 - next_button_->sizeHint().height();
  next_button_->adjustSize();
  skip_button_->adjustSize();
  next_button_->move(card.right() - kMargin - next_button_->width(), by);
  skip_button_->move(card.left() + kMargin, by);
  next_button_->setText(controller_.on_last_step() ? tr("Done") : tr("Next"));
}

void TourOverlay::advance() {
  controller_.next();
  if (!controller_.active()) {
    emit finished();
    return;
  }
  relayout();
  update();
}

void TourOverlay::skip() {
  controller_.skip();
  emit finished();
}

void TourOverlay::paintEvent(QPaintEvent* /*event*/) {
  const TourStep* step = controller_.current();
  if (step == nullptr) {
    return;
  }
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  // Dim the whole app.
  painter.fillRect(rect(), QColor(0, 0, 0, 150));

  const QColor accent = palette().color(QPalette::Highlight);

  // Ring the target button so the eye lands on it through the dim.
  const QRect target = highlight_rect();
  if (!target.isNull()) {
    const QRect ring = target.adjusted(-6, -6, 6, 6);
    painter.setPen(QPen(accent, 2.5));
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 40));
    painter.drawRoundedRect(ring, 8, 8);
  }

  // Coach-mark card.
  const QRect card = card_rect();
  QPainterPath path;
  path.addRoundedRect(card, kRadius, kRadius);
  painter.fillPath(path, palette().color(QPalette::Base));
  painter.setPen(QPen(accent, 1.5));
  painter.drawPath(path);

  const QRect inner = card.adjusted(kMargin, kMargin, -kMargin, -kMargin);
  // Step counter.
  painter.setPen(accent);
  QFont small = font();
  small.setPointSizeF(small.pointSizeF() - 1.0);
  painter.setFont(small);
  painter.drawText(inner,
                   Qt::AlignTop | Qt::AlignRight,
                   tr("Step %1 of %2").arg(controller_.index() + 1).arg(controller_.count()));
  // Title.
  QFont title = font();
  title.setBold(true);
  title.setPointSizeF(title.pointSizeF() + 2.0);
  painter.setFont(title);
  painter.setPen(palette().color(QPalette::WindowText));
  const QRect title_rect(inner.left(), inner.top(), inner.width(), 26);
  painter.drawText(title_rect, Qt::AlignLeft | Qt::AlignTop, step->title);
  // Body (wrapped).
  painter.setFont(font());
  painter.setPen(palette().color(QPalette::WindowText));
  const QRect body_rect(
      inner.left(), title_rect.bottom() + 8, inner.width(), inner.height() - 26 - 8 - 36);
  painter.drawText(body_rect, Qt::TextWordWrap | Qt::AlignTop | Qt::AlignLeft, step->body);
}

} // namespace roadmaker::editor
