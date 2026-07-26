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
  /// Session write policy (#399). Capture sessions (`--screenshot`,
  /// `--screenshot-ui`) must READ the user's settings — a capture renders with
  /// the persisted theme and units — but must never WRITE them: ten scripted
  /// runs otherwise flush every genuine entry out of the recent list through
  /// the kMaxRecentFiles cap, which is how a real user lost theirs. The flag
  /// is process-wide because Settings instances exist in three places
  /// (MainWindow, the reference the WelcomeWidget holds, and main.cpp's theme
  /// lookup) and because a setter added later must be covered by
  /// construction rather than by remembering. Every setter below is a no-op
  /// while it is set; every getter keeps working.
  static void set_read_only(bool read_only);
  [[nodiscard]] static bool read_only();

  /// Scoped form, restoring the previous policy on destruction. Tests must use
  /// this rather than the raw setter: a leaked `true` turns every later case in
  /// the same process into a false green.
  class ReadOnlySession {
  public:
    ReadOnlySession() : previous_(read_only()) { set_read_only(true); }

    ~ReadOnlySession() { set_read_only(previous_); }

    ReadOnlySession(const ReadOnlySession&) = delete;
    ReadOnlySession& operator=(const ReadOnlySession&) = delete;
    ReadOnlySession(ReadOnlySession&&) = delete;
    ReadOnlySession& operator=(ReadOnlySession&&) = delete;

  private:
    bool previous_;
  };

  void save_window(const QMainWindow& window);

  /// Returns false when no saved layout exists yet (first run).
  [[nodiscard]] bool restore_window(QMainWindow& window);

  /// The Library dock's catalogue/files splitter geometry (p6-s7). A splitter
  /// INSIDE a dock is not part of QMainWindow::saveState(), so it needs its own
  /// key or the balance resets on every launch. Empty on first run.
  [[nodiscard]] QByteArray library_splitter_state() const;
  void set_library_splitter_state(const QByteArray& state);

  /// Absolute paths only — see add_recent_file. Entries left behind by a
  /// pre-#399 build that stored a relative path are dropped.
  [[nodiscard]] QStringList recent_files() const;

  /// Prepends `path` (deduplicated, capped at kMaxRecentFiles).
  ///
  /// The path is stored ABSOLUTE. The list is re-read by a LATER process whose
  /// working directory has nothing to do with the one that opened the file — a
  /// Finder/Explorer launch starts at `/` or inside the bundle — so a relative
  /// entry resolves to nothing and the welcome screen's existence filter
  /// silently drops it (#399). Resolving here is correct because this runs in
  /// the process that just opened the file, against the very directory the
  /// path was relative to.
  void add_recent_file(const QString& path);

  /// Recently opened project directories, most recent first (p6-s1). Absolute
  /// paths only, for the same reason as recent_files().
  [[nodiscard]] QStringList recent_projects() const;

  /// Prepends the project directory `path` (deduplicated, absolutized, capped
  /// at kMaxRecentFiles — the same cap as scenes).
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
  /// Shared body of add_recent_file/add_recent_project: absolutize, move to
  /// the front, drop the overflow. Reading through recent_list() means a
  /// pre-#399 relative entry is pruned from the store by the next write.
  void prepend_recent(const char* key, const QString& path);

  /// Stored list for `key`, minus entries that are not absolute paths.
  [[nodiscard]] QStringList recent_list(const char* key) const;

  QSettings settings_;
};

} // namespace roadmaker::editor
