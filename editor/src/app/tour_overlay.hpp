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

// First-run guided-tour overlay (UI-revamp Phase 4). A translucent widget that
// covers the main window, dims the app, highlights the toolbar button the
// current step points at, and shows a themed coach-mark card with Next/Skip.
// Standalone (a child of MainWindow, not the viewport) so it never touches the
// GL overlay. The step logic lives in the headless TourController; this widget
// only paints it and forwards Next/Skip. Never re-shown once finished — the
// caller persists that via Settings.

#include <QWidget>
#include <functional>
#include <vector>

#include "app/tour_controller.hpp"

class QPushButton;

namespace roadmaker::editor {

class TourOverlay : public QWidget {
  Q_OBJECT

public:
  TourOverlay(std::vector<TourStep> steps, QWidget* parent);

  /// Resolves a step's `target` action key to the widget rectangle to
  /// highlight, in this overlay's coordinates (empty rect = no highlight).
  /// MainWindow supplies this from the toolbar's action widgets.
  void set_target_resolver(std::function<QRect(const QString&)> resolver);

  /// Starts the tour and shows the overlay.
  void begin();

  [[nodiscard]] const TourController& controller() const { return controller_; }

signals:
  /// The tour reached the end or was skipped — the caller hides the overlay and
  /// records that it was seen.
  void finished();

protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;

private:
  void relayout();
  void advance();
  void skip();
  [[nodiscard]] QRect highlight_rect() const;
  [[nodiscard]] QRect card_rect() const;

  TourController controller_;
  std::function<QRect(const QString&)> resolver_;
  QPushButton* next_button_;
  QPushButton* skip_button_;
};

} // namespace roadmaker::editor
