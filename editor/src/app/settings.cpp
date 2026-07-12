#include "app/settings.hpp"

namespace roadmaker::editor {

namespace {

const auto* kGeometryKey = "window/geometry";
const auto* kStateKey = "window/state";
const auto* kRecentKey = "files/recent";

} // namespace

void Settings::save_window(const QMainWindow& window) {
  settings_.setValue(kGeometryKey, window.saveGeometry());
  settings_.setValue(kStateKey, window.saveState());
}

bool Settings::restore_window(QMainWindow& window) {
  const QByteArray geometry = settings_.value(kGeometryKey).toByteArray();
  const QByteArray state = settings_.value(kStateKey).toByteArray();
  if (geometry.isEmpty() || state.isEmpty()) {
    return false;
  }
  return window.restoreGeometry(geometry) && window.restoreState(state);
}

QStringList Settings::recent_files() const {
  return settings_.value(kRecentKey).toStringList();
}

void Settings::add_recent_file(const QString& path) {
  QStringList recent = recent_files();
  recent.removeAll(path);
  recent.prepend(path);
  while (recent.size() > kMaxRecentFiles) {
    recent.removeLast();
  }
  settings_.setValue(kRecentKey, recent);
}

bool Settings::autosave_enabled() const {
  return settings_.value(QStringLiteral("autosave/enabled"), true).toBool();
}

void Settings::set_autosave_enabled(bool enabled) {
  settings_.setValue(QStringLiteral("autosave/enabled"), enabled);
}

} // namespace roadmaker::editor
