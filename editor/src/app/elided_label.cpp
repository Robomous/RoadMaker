// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "app/elided_label.hpp"

#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QStyle>
#include <QStyleOption>

namespace roadmaker::editor {

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {
  setTextInteractionFlags(Qt::NoTextInteraction);
}

void ElidedLabel::set_full_text(const QString& text) {
  if (QLabel::text() == text) {
    return;
  }
  // QLabel keeps the full string: it is the source of truth for the size hint
  // and for full_text(); only the painting is elided.
  QLabel::setText(text);
  refresh_tooltip();
}

QString ElidedLabel::displayed_text() const {
  return fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
}

QSize ElidedLabel::minimumSizeHint() const {
  return {0, QLabel::minimumSizeHint().height()};
}

void ElidedLabel::paintEvent(QPaintEvent* /*event*/) {
  QStyleOption option;
  option.initFrom(this);
  QPainter painter(this);
  // Let the style paint the background so the stylesheet still applies; only
  // the text is ours.
  style()->drawPrimitive(QStyle::PE_Widget, &option, &painter, this);
  painter.setPen(palette().color(foregroundRole()));
  painter.drawText(contentsRect(), static_cast<int>(alignment()), displayed_text());
}

void ElidedLabel::resizeEvent(QResizeEvent* event) {
  QLabel::resizeEvent(event);
  refresh_tooltip();
}

void ElidedLabel::refresh_tooltip() {
  setToolTip(displayed_text() == text() ? QString() : text());
}

} // namespace roadmaker::editor
