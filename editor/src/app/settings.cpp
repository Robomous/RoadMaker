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

#include "app/settings.hpp"

#include <QDir>
#include <QFileInfo>

namespace roadmaker::editor {

namespace {

/// Session write policy (#399) — see Settings::set_read_only. A plain
/// process-global: the editor touches settings from the GUI thread only.
bool g_read_only = false;

const auto* kGeometryKey = "window/geometry";
const auto* kStateKey = "window/state";
const auto* kLibrarySplitterKey = "window/library_splitter";
const auto* kRecentKey = "files/recent";
const auto* kRecentProjectsKey = "files/recent_projects";

// Bumped whenever the dockable toolbar/dock STRUCTURE changes so that a layout
// saved by an older RoadMaker is rejected by restoreState (which returns false
// on a version mismatch) rather than misapplied — otherwise a stale saved state
// can park a since-renamed toolbar with a phantom offset. 2 = the flattened
// tabbed toolbar (#374): core strip + one tool row, no nested page toolbars.
constexpr int kWindowStateVersion = 2;

/// A no-op for an already-absolute path; otherwise resolved against the
/// current working directory (#399).
QString absolute(const QString& path) {
  if (path.isEmpty()) {
    return path;
  }
  return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

} // namespace

void Settings::set_read_only(bool read_only) {
  g_read_only = read_only;
}

bool Settings::read_only() {
  return g_read_only;
}

void Settings::save_window(const QMainWindow& window) {
  if (read_only()) {
    return;
  }
  settings_.setValue(kGeometryKey, window.saveGeometry());
  settings_.setValue(kStateKey, window.saveState(kWindowStateVersion));
}

void Settings::set_library_splitter_state(const QByteArray& state) {
  if (read_only()) {
    return;
  }
  settings_.setValue(kLibrarySplitterKey, state);
}

QByteArray Settings::library_splitter_state() const {
  return settings_.value(kLibrarySplitterKey).toByteArray();
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

QStringList Settings::recent_list(const char* key) const {
  QStringList stored = settings_.value(QString::fromLatin1(key)).toStringList();
  // A relative entry can only have been written by a pre-#399 build, and it is
  // unresolvable now: whatever directory it was relative to is long gone
  // (#399). Hiding it here and writing the filtered list back in
  // prepend_recent() prunes it from the store on the next open — no explicit
  // migration step.
  stored.removeIf([](const QString& path) { return !QFileInfo(path).isAbsolute(); });
  return stored;
}

void Settings::prepend_recent(const char* key, const QString& path) {
  if (read_only()) {
    return;
  }
  const QString entry = absolute(path);
  QStringList recent = recent_list(key);
  recent.removeAll(entry);
  recent.prepend(entry);
  while (recent.size() > kMaxRecentFiles) {
    recent.removeLast();
  }
  settings_.setValue(QString::fromLatin1(key), recent);
}

QStringList Settings::recent_files() const {
  return recent_list(kRecentKey);
}

void Settings::add_recent_file(const QString& path) {
  prepend_recent(kRecentKey, path);
}

QStringList Settings::recent_projects() const {
  return recent_list(kRecentProjectsKey);
}

void Settings::add_recent_project(const QString& path) {
  prepend_recent(kRecentProjectsKey, path);
}

QString Settings::theme_name() const {
  return settings_.value(QStringLiteral("ui/theme")).toString();
}

void Settings::set_theme_name(const QString& name) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("ui/theme"), name);
}

bool Settings::autosave_enabled() const {
  return settings_.value(QStringLiteral("autosave/enabled"), true).toBool();
}

void Settings::set_autosave_enabled(bool enabled) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("autosave/enabled"), enabled);
}

bool Settings::tour_seen() const {
  return settings_.value(QStringLiteral("tour/seen"), false).toBool();
}

void Settings::set_tour_seen(bool seen) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("tour/seen"), seen);
}

bool Settings::textured_rendering() const {
  // Default OFF: the editor opens in the plain-color + reference-grid (Sober)
  // look; textured mode (lit surfaces, grass ground, asphalt/concrete) is opt-in.
  return settings_.value(QStringLiteral("view/textured_rendering"), false).toBool();
}

void Settings::set_textured_rendering(bool textured) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("view/textured_rendering"), textured);
}

bool Settings::viewport_hints() const {
  // Default ON: the corner hint is what #103 added for discoverability, and
  // #333 only adds a way OUT of it — an upgrade must not silently take it away.
  return settings_.value(QStringLiteral("view/viewport_hints"), true).toBool();
}

void Settings::set_viewport_hints(bool enabled) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("view/viewport_hints"), enabled);
}

units::UnitSystem Settings::display_units() const {
  // Anything that is not the imperial opt-in reads as the metric default —
  // including values written by a future version this build does not know.
  const QString stored =
      settings_.value(QStringLiteral("view/display_units"), QStringLiteral("metric")).toString();
  return stored == QLatin1String("imperial") ? units::UnitSystem::Imperial
                                             : units::UnitSystem::Metric;
}

void Settings::set_display_units(units::UnitSystem system) {
  if (read_only()) {
    return;
  }
  settings_.setValue(QStringLiteral("view/display_units"),
                     system == units::UnitSystem::Imperial ? QStringLiteral("imperial")
                                                           : QStringLiteral("metric"));
}

} // namespace roadmaker::editor
