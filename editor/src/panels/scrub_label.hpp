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

// Scrub-editing (P1/GW-2): drag an attribute's NAME horizontally to change its
// value live. The label is a pure gesture source — it knows pixels and
// modifiers, never units, values, or commands. Its consumer (PropertiesPanel)
// maps the emitted pixel delta onto the attribute's units and drives ONE
// preview session per gesture, so a whole scrub lands as a single undo entry.

#include <QLabel>
#include <QPoint>

namespace roadmaker::editor {

/// A QLabel that turns a horizontal drag into a running, modifier-scaled pixel
/// delta.
class ScrubLabel : public QLabel {
  Q_OBJECT

public:
  explicit ScrubLabel(const QString& text, QWidget* parent = nullptr);

  /// Precision modifiers, applied to the motion that happens WHILE they are
  /// held — never retroactively, so tapping ⇧ mid-drag refines from where the
  /// value already is instead of snapping it somewhere new.
  static constexpr double kFineMultiplier = 0.1;    ///< ⇧
  static constexpr double kCoarseMultiplier = 10.0; ///< Ctrl / ⌘

  /// Pixels the press may drift before it counts as a scrub rather than a
  /// click. Below it nothing is emitted, so clicking a label stays inert.
  static constexpr int kDragSlop = 3;

  [[nodiscard]] bool scrubbing() const { return scrubbing_; }

signals:
  /// The drag passed the slop: the consumer snapshots its baseline value and
  /// opens a preview session.
  void scrub_started();

  /// Running total of modifier-scaled pixels since the gesture began (right is
  /// positive). The consumer applies `baseline + delta * units_per_pixel`.
  void scrub_moved(double delta);

  /// The drag ended — commit the session as one undo entry.
  void scrub_finished();

  /// Esc, or a lost grab: restore the baseline and cancel the session.
  void scrub_cancelled();

protected:
  void mousePressEvent(QMouseEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;

private:
  /// Ends an active gesture, emitting cancelled (or nothing when idle).
  void abort();

  [[nodiscard]] static double multiplier_for(Qt::KeyboardModifiers modifiers);

  bool pressed_ = false;   ///< button down, gesture may not have started yet
  bool scrubbing_ = false; ///< past the slop: emitting deltas
  QPoint press_pos_;
  QPoint last_pos_;
  double delta_ = 0.0; ///< accumulated scaled pixels
};

} // namespace roadmaker::editor
