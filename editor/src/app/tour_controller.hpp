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

// First-run guided tour state machine (UI-revamp Phase 4). Headless and
// widget-free — it holds an ordered list of steps and a cursor, so the step
// logic (start / advance / skip / done) is unit-testable without a QWidget.
// The overlay widget owns one of these, paints current(), and drives it from
// Next/Skip; MainWindow maps each step's `target` to a real toolbar action so
// the tour highlights an actual button. "Seen" persistence lives in Settings,
// not here.

#include <QString>
#include <cstddef>
#include <vector>

namespace roadmaker::editor {

/// One coach-mark: a title + body, and the `target` key of the toolbar action
/// it points at (empty = a centred card with no highlight). The key matches an
/// action's iconText (e.g. "Road", "Library", "Elevation", "Export").
struct TourStep {
  QString title;
  QString body;
  QString target;
};

class TourController {
public:
  explicit TourController(std::vector<TourStep> steps);

  [[nodiscard]] const std::vector<TourStep>& steps() const { return steps_; }

  /// Begins at the first step. A tour with no steps starts already finished.
  void start();

  /// True while a step is showing (between start() and running past the end or
  /// a skip()).
  [[nodiscard]] bool active() const { return active_; }

  /// The showing step, or nullptr when inactive/finished.
  [[nodiscard]] const TourStep* current() const;

  /// 0-based index of the showing step (0 when inactive).
  [[nodiscard]] std::size_t index() const { return index_; }

  /// Total step count (for "Step i of n").
  [[nodiscard]] std::size_t count() const { return steps_.size(); }

  /// Advances to the next step; advancing off the last step finishes the tour
  /// (active() → false, completed() → true).
  void next();

  /// Ends the tour immediately (active() → false, completed() → true).
  void skip();

  /// True once the tour ran to the end or was skipped (drives the "seen" flag).
  [[nodiscard]] bool completed() const { return completed_; }

  /// True on the last step (the overlay shows "Done" instead of "Next").
  [[nodiscard]] bool on_last_step() const { return active_ && index_ + 1 >= steps_.size(); }

private:
  std::vector<TourStep> steps_;
  std::size_t index_ = 0;
  bool active_ = false;
  bool completed_ = false;
};

/// The bundled 5-step first-run tour (create road → drag a T → place a tree →
/// edit elevation → export). Targets match toolbar action iconTexts.
[[nodiscard]] std::vector<TourStep> default_tour_steps();

} // namespace roadmaker::editor
