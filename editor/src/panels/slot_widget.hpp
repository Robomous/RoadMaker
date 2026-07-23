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

// Asset/material reference slots (P1/GW-3): a framed drop target in the
// Attributes pane that names what it points at, accepts a Library drag, and
// can send the user to the right Library category when engaged.
//
// The widget is a pure UI seam — it resolves nothing and commits nothing. It
// emits the dropped key and lets its consumer decide what that key means and
// which command records it, so the same slot serves props, materials, and
// whatever P4/P6 hang off it later.

#include <QFrame>
#include <QString>

class QLabel;

namespace roadmaker::editor {

/// A labelled drop target for one library reference.
class SlotWidget : public QFrame {
  Q_OBJECT

public:
  /// `category` is the Library category this slot draws from — it is what
  /// engage_requested carries, and it never leaves the widget otherwise.
  explicit SlotWidget(QString category, QWidget* parent = nullptr);

  /// Sets the shown reference: `label` is the human name (empty → the
  /// placeholder), `thumbnail` an image path (empty → no preview).
  void set_item(const QString& label, const QString& thumbnail = {});

  [[nodiscard]] QString category() const { return category_; }

signals:
  /// A library item was dropped. The consumer resolves the key and pushes the
  /// command that records it.
  void item_dropped(const QString& key);

  /// The user clicked the slot: take them to `category` in the Library.
  void engage_requested(const QString& category);

protected:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dragLeaveEvent(QDragLeaveEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;

private:
  /// True when the drag carries exactly the Library's item MIME type.
  [[nodiscard]] static bool has_library_mime(const QDropEvent* event);

  /// Paints the hover affordance (or clears it) — the slot must look like it
  /// will take the drop before the user lets go.
  void set_drag_hovered(bool hovered);

  QString category_;
  QLabel* preview_;
  QLabel* caption_;
  bool drag_hovered_ = false;
};

} // namespace roadmaker::editor
