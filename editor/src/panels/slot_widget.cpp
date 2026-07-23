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

#include "panels/slot_widget.hpp"

#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QPixmap>
#include <QVBoxLayout>

#include "document/library_list_model.hpp"
#include "theme/theme.hpp"

namespace roadmaker::editor {

namespace {

constexpr int kPreviewSide = 48;

} // namespace

SlotWidget::SlotWidget(QString category, QWidget* parent)
    : QFrame(parent), category_(std::move(category)), preview_(new QLabel(this)),
      caption_(new QLabel(this)) {
  setObjectName(QStringLiteral("slot_%1").arg(category_));
  setFrameShape(QFrame::StyledPanel);
  setAcceptDrops(true);
  setCursor(Qt::PointingHandCursor);
  setToolTip(tr("Drop a %1 from the Library, or click to browse.").arg(category_));

  preview_->setFixedSize(kPreviewSide, kPreviewSide);
  preview_->setAlignment(Qt::AlignCenter);
  preview_->setFrameShape(QFrame::NoFrame);

  caption_->setWordWrap(true);

  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(6, 6, 6, 6);
  layout->addWidget(preview_);
  layout->addWidget(caption_, 1);

  set_item({});
  set_drag_hovered(false);
}

void SlotWidget::set_item(const QString& label, const QString& thumbnail) {
  caption_->setText(label.isEmpty() ? tr("(none)") : label);
  caption_->setEnabled(!label.isEmpty()); // the placeholder reads as inactive

  QPixmap pixmap(thumbnail);
  if (thumbnail.isEmpty() || pixmap.isNull()) {
    // No preview to show: hide the box rather than reserve a blank square that
    // reads as a broken image.
    preview_->clear();
    preview_->hide();
    return;
  }
  preview_->setPixmap(
      pixmap.scaled(kPreviewSide, kPreviewSide, Qt::KeepAspectRatio, Qt::SmoothTransformation));
  preview_->show();
}

bool SlotWidget::has_library_mime(const QDropEvent* event) {
  return event->mimeData()->hasFormat(QString::fromLatin1(kLibraryItemMimeType));
}

void SlotWidget::set_drag_hovered(bool hovered) {
  drag_hovered_ = hovered;
  // Themed accent border while a droppable item is over the slot; the resting
  // border keeps the frame's own colour.
  const QColor accent = theme::current().accent;
  setStyleSheet(
      hovered
          ? QStringLiteral("QFrame#%1 { border: 2px solid %2; }").arg(objectName(), accent.name())
          : QString());
}

void SlotWidget::dragEnterEvent(QDragEnterEvent* event) {
  if (!has_library_mime(event)) {
    return; // ignored: the drag falls through to whatever is behind us
  }
  event->acceptProposedAction();
  set_drag_hovered(true);
}

void SlotWidget::dragLeaveEvent(QDragLeaveEvent* event) {
  set_drag_hovered(false);
  QFrame::dragLeaveEvent(event);
}

void SlotWidget::dropEvent(QDropEvent* event) {
  set_drag_hovered(false);
  if (!has_library_mime(event)) {
    return;
  }
  const QString key =
      QString::fromUtf8(event->mimeData()->data(QString::fromLatin1(kLibraryItemMimeType)));
  if (key.isEmpty()) {
    return; // a malformed payload is not a drop
  }
  event->acceptProposedAction();
  emit item_dropped(key);
}

void SlotWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton && rect().contains(event->pos())) {
    emit engage_requested(category_);
    event->accept();
    return;
  }
  QFrame::mouseReleaseEvent(event);
}

} // namespace roadmaker::editor
