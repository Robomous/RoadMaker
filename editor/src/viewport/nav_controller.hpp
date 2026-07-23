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

// Viewport navigation chords (P1/GW-1). A pure, headless state machine: it
// owns NO camera, widget, or GL state — ViewportWidget feeds it raw
// button/modifier events, asks which gesture is live, and applies the matching
// OrbitCamera math itself. That split is what makes the chord matrix testable
// without a window (editor/tests/test_nav_controller.cpp).

#include <QPoint>
#include <Qt>

namespace roadmaker::editor {

/// The navigation gesture a chord resolves to.
enum class NavGesture {
  None,           ///< nothing navigational — the press belongs to the gizmo/tool
  Orbit,          ///< ⌥+LMB: polar orbit around the pivot
  LegacyOrbit,    ///< RMB drag without ⌥ — the pre-P1 binding, kept as an alternate
  ContextPending, ///< RMB down without ⌥, still short of the drag threshold
  Pan,            ///< ⌥+LMB+RMB or MMB: move camera + pivot together
  ZoomDrag,       ///< ⌥+RMB: drag up = zoom in
  PivotVertical,  ///< ⌥+⇧+LMB+RMB: raise/lower the pivot
};

/// Resolves the button/modifier combination held right now to its gesture.
/// Free function so the chord matrix is one readable table and can be tested
/// without driving events through the state machine.
[[nodiscard]] NavGesture chord_for(Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers);

/// Tracks viewport navigation chords across press/move/release.
///
/// The gesture is latched at BUTTON transitions from the modifiers held at that
/// instant, and does not re-resolve on move. Releasing ⌥ mid-orbit therefore
/// finishes the orbit instead of killing it, which is what a user dragging
/// across a screen expects; the cost is that adding ⇧ mid-pan does not upgrade
/// it to a pivot lift — press the chord again for that.
class NavController {
public:
  /// Pixels an RMB press may drift before it stops being a context click and
  /// becomes a legacy orbit. Scaled by the caller for the device pixel ratio.
  static constexpr int kContextDragSlop = 4;

  /// Feeds a button press at `pos`. Returns true when navigation claims the
  /// event: the caller must NOT forward a claimed press to the gizmo or the
  /// active tool. A plain (unmodified) LMB press is never claimed.
  bool press(Qt::MouseButton button,
             Qt::MouseButtons buttons,
             Qt::KeyboardModifiers modifiers,
             const QPoint& pos);

  /// Feeds a move at `pos` with the buttons still held. Returns true when a
  /// gesture is live and consumed it. `slop_px` is kContextDragSlop scaled for
  /// the device pixel ratio: a ContextPending press that drifts past it becomes
  /// a LegacyOrbit.
  ///
  /// A move with no buttons held ends any latched gesture. That is the recovery
  /// path for a release we never saw — a window manager that grabs ⌥+drag to
  /// move the window, or focus lost mid-chord — which would otherwise leave the
  /// camera orbiting a button nobody is pressing.
  bool move(const QPoint& pos, Qt::MouseButtons buttons, int slop_px);

  /// Feeds a button release. Returns true when navigation claims the event.
  /// Sets `*context_click` when the release ends an RMB press that never
  /// dragged — the caller pops the context menu.
  bool release(Qt::MouseButton button, Qt::MouseButtons buttons, bool* context_click);

  /// Abandons any in-flight gesture (focus loss, Esc, a synthetic interruption).
  void reset() { gesture_ = NavGesture::None; }

  [[nodiscard]] NavGesture gesture() const { return gesture_; }

  /// True while a gesture owns the mouse, so hover/tool routing stays quiet.
  /// ContextPending does not: a right-click that never drags must leave hover
  /// exactly as it was.
  [[nodiscard]] bool navigating() const {
    return gesture_ != NavGesture::None && gesture_ != NavGesture::ContextPending;
  }

private:
  NavGesture gesture_ = NavGesture::None;

  /// Modifiers latched at the last press. release() re-resolves the chord with
  /// them, so lifting one button of a two-button chord unwinds to the gesture
  /// the remaining button implies (⌥+LMB+RMB pan → release RMB → ⌥+LMB orbit)
  /// without needing the live modifier state a release event doesn't carry.
  Qt::KeyboardModifiers modifiers_ = Qt::NoModifier;

  /// Where a ContextPending press went down, for the slop test.
  QPoint press_pos_;
};

} // namespace roadmaker::editor
