#pragma once

// QSettings wrapper: window geometry/dock layout persistence and the
// recent-files list. Storage location is platform-native (registry / plist /
// INI under XDG) — never a hardcoded path.

#include <QMainWindow>
#include <QSettings>
#include <QStringList>

namespace roadmaker::editor {

class Settings {
public:
  void save_window(const QMainWindow& window);

  /// Returns false when no saved layout exists yet (first run).
  [[nodiscard]] bool restore_window(QMainWindow& window);

  [[nodiscard]] QStringList recent_files() const;

  /// Prepends `path` (deduplicated, capped at kMaxRecentFiles).
  void add_recent_file(const QString& path);

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

  static constexpr int kMaxRecentFiles = 10;

private:
  QSettings settings_;
};

} // namespace roadmaker::editor
