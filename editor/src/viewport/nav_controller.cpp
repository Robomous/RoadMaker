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

#include "viewport/nav_controller.hpp"

namespace roadmaker::editor {

NavGesture chord_for(Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers) {
  const bool alt = modifiers.testFlag(Qt::AltModifier);
  const bool shift = modifiers.testFlag(Qt::ShiftModifier);
  const bool left = buttons.testFlag(Qt::LeftButton);
  const bool right = buttons.testFlag(Qt::RightButton);
  const bool middle = buttons.testFlag(Qt::MiddleButton);

  // Order matters: the two-button ⌥ chords must be tested before the
  // single-button ones they are built from, or ⌥+LMB+RMB would read as orbit.
  if (middle) {
    return NavGesture::Pan; // MMB pans with or without ⌥ (the no-⌥ alternate)
  }
  if (alt && left && right) {
    return shift ? NavGesture::PivotVertical : NavGesture::Pan;
  }
  if (alt && left) {
    return NavGesture::Orbit;
  }
  if (alt && right) {
    return NavGesture::ZoomDrag;
  }
  if (right) {
    return NavGesture::ContextPending; // pre-P1 binding: menu, or orbit past the slop
  }
  return NavGesture::None; // plain LMB — the gizmo/tool path
}

bool NavController::press(Qt::MouseButton button,
                          Qt::MouseButtons buttons,
                          Qt::KeyboardModifiers modifiers,
                          const QPoint& pos) {
  modifiers_ = modifiers;
  const NavGesture next = chord_for(buttons, modifiers);
  if (next == NavGesture::None) {
    return false; // leaves gesture_ alone: nothing navigational is in flight
  }
  if (button == Qt::RightButton && next == NavGesture::ContextPending) {
    press_pos_ = pos;
  }
  gesture_ = next;
  return true;
}

bool NavController::move(const QPoint& pos, Qt::MouseButtons buttons, int slop_px) {
  if (buttons == Qt::NoButton) {
    gesture_ = NavGesture::None; // a release we never saw — drop the chord
    return false;
  }
  if (gesture_ == NavGesture::ContextPending) {
    // Below the slop this is still a click: consume nothing, so hover and the
    // active tool behave exactly as they did before the button went down.
    if ((pos - press_pos_).manhattanLength() <= slop_px) {
      return false;
    }
    gesture_ = NavGesture::LegacyOrbit;
  }
  return navigating();
}

bool NavController::release(Qt::MouseButton button, Qt::MouseButtons buttons, bool* context_click) {
  if (context_click != nullptr) {
    *context_click = false;
  }
  if (gesture_ == NavGesture::None) {
    return false; // the press was never ours — the tool owns this release
  }
  const bool ends_context_click =
      gesture_ == NavGesture::ContextPending && button == Qt::RightButton;
  const bool claimed = navigating() || ends_context_click;
  if (ends_context_click && context_click != nullptr) {
    *context_click = true;
  }
  // Re-resolve against what is still held, so a chord unwinds one button at a
  // time (⌥+LMB+RMB pan → release RMB → ⌥+LMB orbit → release LMB → idle).
  gesture_ = chord_for(buttons, modifiers_);
  return claimed;
}

} // namespace roadmaker::editor
