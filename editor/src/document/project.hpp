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
// (`{"project_version": 2, "name": "...", "last_scene": "main.xodr"}`) beside its scenes as
// ordinary top-level `*.xodr` files, with an optional `assets/library/manifest.json` overlaying the
// built-in Library catalogue while the project is open. No new scene format, no registry, no
// database — scenes are discovered by glob and stay openable standalone outside any project.
// Headless (QtCore JSON only, no widget), unit-testable offscreen. The schema is versioned and
// forward-compatible like LibraryManifest: a newer project_version parses the
// fields this build knows (a warning, not an error) and unknown keys survive a
// rewrite verbatim, so a future manifest never bricks — nor is damaged by — an
// older editor.
//
// v2 (fmt-s1, #325) is the project half of ADR-0008's Layer-2 container: it
// adds `last_scene` and the atomic save() that writes it. Everything the scene
// itself carries lives in `<scene>.rmscene.json` (document/scene_sidecar.hpp),
// never here.

#include "roadmaker/error.hpp"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <filesystem>
#include <optional>

namespace roadmaker::editor {

class Project {
public:
  /// The manifest schema this build understands; higher versions parse
  /// best-effort with a warning.
  static constexpr int kSupportedVersion = 2;

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

  /// Rewrites project.json atomically, MERGING over the manifest as parsed —
  /// every key this build does not model survives (unlike v1, which rebuilt the
  /// file from two fields and would have dropped them).
  ///
  /// Non-const because it bumps version_ to kSupportedVersion on the first v2
  /// write; a manifest from a NEWER build keeps its own version rather than
  /// being downgraded. A name that was only inferred from the directory is not
  /// materialized — writing it would turn a clean two-key manifest into a
  /// gratuitous diff.
  [[nodiscard]] Expected<void> save();

  [[nodiscard]] const QString& name() const { return name_; }

  [[nodiscard]] const std::filesystem::path& dir() const { return dir_; }

  [[nodiscard]] int version() const { return version_; }

  /// The project's scenes: absolute paths of the top-level `*.xodr` files in
  /// the project directory (non-recursive), sorted by file name. Re-globbed on
  /// every call so it reflects the directory as it is now.
  [[nodiscard]] QStringList scenes() const;

  /// The project's asset root, `<dir>/assets` — the folder the Library's file
  /// explorer browses (p6-s7) and the parent of the Library overlay. Returned
  /// unconditionally, whether or not it exists on disk: the browser watches for
  /// it to appear. NOTHING in RoadMaker creates it; a project only grows one
  /// once the user, or an asset commit, puts something there.
  /// The scene the project was last working on, as stored: a project-relative
  /// file name with `/` separators, or empty. See last_scene_path() to use it.
  [[nodiscard]] const QString& last_scene() const { return last_scene_; }

  /// Records `scene` as the last scene. A path outside the project (a Save As
  /// elsewhere) CLEARS the field rather than storing a `../` escape: the
  /// manifest must stay portable, and a project may only point at its own
  /// scenes. Takes effect on disk at the next save().
  void set_last_scene(const std::filesystem::path& scene);

  /// The absolute last-scene path, or nullopt when none is stored or the file
  /// is gone (renamed, deleted, or never committed alongside the manifest).
  /// Existence-checked here so reopening a project can simply skip it.
  [[nodiscard]] std::optional<std::filesystem::path> last_scene_path() const;

  [[nodiscard]] std::filesystem::path assets_dir() const;

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
  QString last_scene_;
  /// True when name_ was inferred from the directory rather than read from the
  /// manifest — save() then leaves the key absent instead of baking it in.
  bool name_is_fallback_ = false;
  /// The manifest as parsed, the merge base for save(). Empty for a Project
  /// that never came off disk.
  QJsonObject raw_;
};

} // namespace roadmaker::editor
