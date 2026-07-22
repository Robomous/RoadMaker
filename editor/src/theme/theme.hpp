// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Theme token system (docs/standards/ui-design.md). Every editor chrome
// color flows from these tokens: Fusion style + QPalette + one generated
// QSS string. Widgets never hardcode colors. The viewport backdrop takes
// the same tokens as plain floats (BackdropColors) so render/ stays
// Qt-free.

#include <QColor>
#include <QString>
#include <QStringList>

#include "render/renderer.hpp"

class QApplication;

namespace roadmaker::editor {

struct Theme {
  QString name;         // kebab-case id: CLI --theme value and QSettings key
  QString display_name; // human-readable, for a future View menu entry

  // Background layers, shallowest widget chrome first (ui-design.md table).
  QColor bg0;      // deepest: welcome backdrop, viewport surround
  QColor bg1;      // panels, docks, menus (QPalette::Window)
  QColor bg2;      // elevated chrome: toolbar, dock titles, cards
  QColor bg_input; // inputs and item views (QPalette::Base)

  QColor border;
  QColor border_strong;

  QColor text_primary;
  QColor text_secondary;
  QColor text_disabled;

  QColor accent;
  QColor accent_hover;
  QColor on_accent;

  QColor warning;
  QColor error;
  QColor success;

  // Viewport backdrop.
  QColor sky_top;
  QColor sky_horizon;
  QColor grid_major;
  QColor grid_minor;

  /// Tokens as plain floats for Renderer::set_backdrop().
  [[nodiscard]] BackdropColors backdrop() const;
};

namespace theme {

/// The three Phase 0 candidate palettes (maintainer picks one; the pick
/// becomes default_theme()).
[[nodiscard]] const Theme& graphite_amber();
[[nodiscard]] const Theme& slate_cyan();
[[nodiscard]] const Theme& warm_signal();

[[nodiscard]] const Theme& default_theme();

/// Theme ids accepted by --theme / the settings key.
[[nodiscard]] QStringList available();

/// Lookup by id; nullptr when unknown (caller falls back to default_theme).
[[nodiscard]] const Theme* by_name(const QString& name);

/// Applies Fusion style + QPalette + generated QSS app-wide and records the
/// theme as current(). Call once before the first window is constructed;
/// calling again re-themes live (icons re-tint via the palette-change
/// event, MainWindow::changeEvent).
void apply(QApplication& app, const Theme& theme);

/// The last theme passed to apply(); default_theme() before any apply().
[[nodiscard]] const Theme& current();

} // namespace theme

} // namespace roadmaker::editor
