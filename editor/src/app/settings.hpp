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

// QSettings wrapper: window geometry/dock layout persistence and the
// recent-files list. Storage location is platform-native (registry / plist /
// INI under XDG) — never a hardcoded path.

#include <QMainWindow>
#include <QSettings>
#include <QStringList>

#include "document/units.hpp"

namespace roadmaker::editor {

class Settings {
public:
  void save_window(const QMainWindow& window);

  /// Returns false when no saved layout exists yet (first run).
  [[nodiscard]] bool restore_window(QMainWindow& window);

  [[nodiscard]] QStringList recent_files() const;

  /// Prepends `path` (deduplicated, capped at kMaxRecentFiles).
  void add_recent_file(const QString& path);

  /// Recently opened project directories, most recent first (p6-s1).
  [[nodiscard]] QStringList recent_projects() const;

  /// Prepends the project directory `path` (deduplicated, capped at
  /// kMaxRecentFiles — the same cap as scenes).
  void add_recent_project(const QString& path);

  /// Theme id (docs/standards/ui-design.md). Empty = default_theme(); the
  /// --theme CLI flag overrides without persisting.
  [[nodiscard]] QString theme_name() const;
  void set_theme_name(const QString& name);

  /// Autosave master switch (hardening §4.6 "setting to disable").
  [[nodiscard]] bool autosave_enabled() const;
  void set_autosave_enabled(bool enabled);

  /// Whether the first-run guided tour has already been shown (or skipped).
  /// Persisted so the tour never re-appears; no telemetry.
  [[nodiscard]] bool tour_seen() const;
  void set_tour_seen(bool seen);

  /// Viewport render mode: true = daytime Textured (default), false = flat
  /// Sober (the M2 look / packaging smoke path). docs/design/m3a/04_render.md §5.
  [[nodiscard]] bool textured_rendering() const;
  void set_textured_rendering(bool textured);

  /// Viewport corner hint (View ▸ Viewport Hints, issue #333): true = the
  /// active tool's instruction is drawn in the viewport corner (the default —
  /// #103 discoverability), false = viewport only, status bar unaffected.
  [[nodiscard]] bool viewport_hints() const;
  void set_viewport_hints(bool enabled);

  /// Display units (#412): metric (the default) or imperial. Display + input
  /// parsing only — files, commands and the kernel stay SI meters
  /// (docs/domain/realism_defaults.md, Unit policy), so this never dirties a
  /// document.
  [[nodiscard]] units::UnitSystem display_units() const;
  void set_display_units(units::UnitSystem system);

  static constexpr int kMaxRecentFiles = 10;

private:
  QSettings settings_;
};

} // namespace roadmaker::editor
