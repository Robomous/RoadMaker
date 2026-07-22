#include "app/settings.hpp"

namespace roadmaker::editor {

namespace {

const auto* kGeometryKey = "window/geometry";
const auto* kStateKey = "window/state";
const auto* kRecentKey = "files/recent";
const auto* kRecentProjectsKey = "files/recent_projects";

// Bumped whenever the dockable toolbar/dock STRUCTURE changes so that a layout
// saved by an older RoadMaker is rejected by restoreState (which returns false
// on a version mismatch) rather than misapplied — otherwise a stale saved state
// can park a since-renamed toolbar with a phantom offset. 2 = the flattened
// tabbed toolbar (#374): core strip + one tool row, no nested page toolbars.
constexpr int kWindowStateVersion = 2;

} // namespace

void Settings::save_window(const QMainWindow& window) {
  settings_.setValue(kGeometryKey, window.saveGeometry());
  settings_.setValue(kStateKey, window.saveState(kWindowStateVersion));
}

bool Settings::restore_window(QMainWindow& window) {
  const QByteArray geometry = settings_.value(kGeometryKey).toByteArray();
  const QByteArray state = settings_.value(kStateKey).toByteArray();
  if (geometry.isEmpty() || state.isEmpty()) {
    return false;
  }
  // restoreState returns false on a version mismatch, so an out-of-date saved
  // layout is discarded and the caller falls back to the default arrangement.
  return window.restoreGeometry(geometry) && window.restoreState(state, kWindowStateVersion);
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

QStringList Settings::recent_projects() const {
  return settings_.value(kRecentProjectsKey).toStringList();
}

void Settings::add_recent_project(const QString& path) {
  QStringList recent = recent_projects();
  recent.removeAll(path);
  recent.prepend(path);
  while (recent.size() > kMaxRecentFiles) {
    recent.removeLast();
  }
  settings_.setValue(kRecentProjectsKey, recent);
}

QString Settings::theme_name() const {
  return settings_.value(QStringLiteral("ui/theme")).toString();
}

void Settings::set_theme_name(const QString& name) {
  settings_.setValue(QStringLiteral("ui/theme"), name);
}

bool Settings::autosave_enabled() const {
  return settings_.value(QStringLiteral("autosave/enabled"), true).toBool();
}

void Settings::set_autosave_enabled(bool enabled) {
  settings_.setValue(QStringLiteral("autosave/enabled"), enabled);
}

bool Settings::tour_seen() const {
  return settings_.value(QStringLiteral("tour/seen"), false).toBool();
}

void Settings::set_tour_seen(bool seen) {
  settings_.setValue(QStringLiteral("tour/seen"), seen);
}

bool Settings::textured_rendering() const {
  // Default OFF: the editor opens in the plain-color + reference-grid (Sober)
  // look; textured mode (lit surfaces, grass ground, asphalt/concrete) is opt-in.
  return settings_.value(QStringLiteral("view/textured_rendering"), false).toBool();
}

void Settings::set_textured_rendering(bool textured) {
  settings_.setValue(QStringLiteral("view/textured_rendering"), textured);
}

bool Settings::viewport_hints() const {
  // Default ON: the corner hint is what #103 added for discoverability, and
  // #333 only adds a way OUT of it — an upgrade must not silently take it away.
  return settings_.value(QStringLiteral("view/viewport_hints"), true).toBool();
}

void Settings::set_viewport_hints(bool enabled) {
  settings_.setValue(QStringLiteral("view/viewport_hints"), enabled);
}

int Settings::toolbar_tab() const {
  return settings_.value(QStringLiteral("ui/toolbar_tab"), 0).toInt();
}

void Settings::set_toolbar_tab(int index) {
  settings_.setValue(QStringLiteral("ui/toolbar_tab"), index);
}

} // namespace roadmaker::editor
