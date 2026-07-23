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

// A project is a directory (p6-s1, #235): a `project.json` manifest
// (`{"project_version": 1, "name": "..."}`) beside its scenes as ordinary
// top-level `*.xodr` files, with an optional `assets/library/manifest.json`
// overlaying the built-in Library catalogue while the project is open. No new
// scene format, no registry, no database — scenes are discovered by glob and
// stay openable standalone outside any project. Headless (QtCore JSON only, no
// widget), unit-testable offscreen. The schema is versioned and
// forward-compatible like LibraryManifest: a newer project_version parses the
// fields this build knows (a warning, not an error) and unknown keys are
// ignored, so a future manifest never bricks an older editor.

#include "roadmaker/error.hpp"

#include <QString>
#include <QStringList>
#include <filesystem>
#include <optional>

namespace roadmaker::editor {

class Project {
public:
  /// The manifest schema this build understands; higher versions parse
  /// best-effort with a warning.
  static constexpr int kSupportedVersion = 1;

  /// The manifest file marking a directory as a project.
  static constexpr const char* kManifestName = "project.json";

  /// Creates `dir` (and parents) if needed and writes a fresh project.json.
  /// Errors when the directory already holds a project.json — creating must
  /// never clobber an existing project.
  [[nodiscard]] static Expected<Project> create(const std::filesystem::path& dir,
                                                const QString& name);

  /// Opens the project at `dir` by parsing and validating its project.json.
  /// Errors: no manifest, malformed JSON, or a missing/invalid integer
  /// `project_version`. A missing/empty `name` falls back to the directory
  /// name so every project has something to show in the UI.
  [[nodiscard]] static Expected<Project> open(const std::filesystem::path& dir);

  [[nodiscard]] const QString& name() const { return name_; }

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

  [[nodiscard]] int version() const { return version_; }

  /// The project's scenes: absolute paths of the top-level `*.xodr` files in
  /// the project directory (non-recursive), sorted by file name. Re-globbed on
  /// every call so it reflects the directory as it is now.
  [[nodiscard]] QStringList scenes() const;

  /// `<dir>/assets/library/manifest.json` when that file exists — the
  /// per-project Library overlay (same schema as the built-in qrc manifest).
  [[nodiscard]] std::optional<std::filesystem::path> library_manifest_path() const;

  /// The project directory containing `scene_path`, or nullopt. Deliberately
  /// checks ONLY the scene's immediate parent directory for a project.json
  /// (no upward walk): scenes are top-level files of their project directory,
  /// so a deeper hit would be a different project's file, not this scene's.
  [[nodiscard]] static std::optional<std::filesystem::path>
  find_project_for(const std::filesystem::path& scene_path);

private:
  QString name_;
  std::filesystem::path dir_;
  int version_ = kSupportedVersion;
};

} // namespace roadmaker::editor
