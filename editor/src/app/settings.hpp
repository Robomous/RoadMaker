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

  /// Autosave master switch (hardening §4.6 "setting to disable").
  [[nodiscard]] bool autosave_enabled() const;
  void set_autosave_enabled(bool enabled);

  static constexpr int kMaxRecentFiles = 10;

private:
  QSettings settings_;
};

} // namespace roadmaker::editor
