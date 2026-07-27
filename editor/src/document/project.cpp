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

#include "document/project.hpp"

#include <spdlog/spdlog.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <algorithm>
#include <system_error>

namespace roadmaker::editor {

namespace {

std::filesystem::path manifest_path(const std::filesystem::path& dir) {
  return dir / Project::kManifestName;
}

QString qpath(const std::filesystem::path& path) {
  return QString::fromStdString(path.string());
}

} // namespace

Expected<Project> Project::create(const std::filesystem::path& dir, const QString& name) {
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure, "cannot create the project directory", dir.string());
  }
  const std::filesystem::path manifest = manifest_path(dir);
  if (std::filesystem::exists(manifest, ec)) {
    return make_error(
        ErrorCode::InvalidArgument, "the directory already contains a project.json", dir.string());
  }
  QJsonObject root;
  root.insert(QStringLiteral("project_version"), kSupportedVersion);
  root.insert(QStringLiteral("name"), name);
  // QSaveFile: the manifest appears atomically or not at all — a half-written
  // project.json must never mark a directory as a (broken) project.
  QSaveFile file(qpath(manifest));
  if (!file.open(QIODevice::WriteOnly)) {
    return make_error(ErrorCode::IoFailure, "cannot write project.json", manifest.string());
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    return make_error(ErrorCode::IoFailure, "cannot write project.json", manifest.string());
  }
  return open(dir);
}

Expected<Project> Project::open(const std::filesystem::path& dir) {
  const std::filesystem::path manifest = manifest_path(dir);
  QFile file(qpath(manifest));
  if (!file.open(QIODevice::ReadOnly)) {
    return make_error(ErrorCode::FileNotFound, "cannot read project.json", manifest.string());
  }
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
  if (error.error != QJsonParseError::NoError) {
    return make_error(ErrorCode::InvalidArgument,
                      "project.json is not valid JSON: " + error.errorString().toStdString(),
                      manifest.string());
  }
  if (!document.isObject()) {
    return make_error(
        ErrorCode::InvalidArgument, "project.json root is not an object", manifest.string());
  }
  const QJsonObject root = document.object();
  const QJsonValue version = root.value(QStringLiteral("project_version"));
  if (!version.isDouble()) {
    return make_error(ErrorCode::InvalidArgument,
                      "project.json is missing an integer project_version",
                      manifest.string());
  }

  Project project;
  project.version_ = version.toInt();
  // Forward compatibility (the LibraryManifest rule): a newer schema still
  // parses the fields this build knows; only warn so a future project never
  // bricks an older editor. Unknown keys are ignored by construction.
  if (project.version_ > kSupportedVersion) {
    spdlog::warn("project.json version {} is newer than supported version {} — "
                 "parsing known fields only",
                 project.version_,
                 kSupportedVersion);
  }
  // Normalize so two routes to the same directory compare equal (the editor
  // compares dirs to decide whether a scene switches projects).
  std::error_code ec;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(dir, ec);
  project.dir_ = ec ? dir : canonical;
  project.name_ = root.value(QStringLiteral("name")).toString();
  if (project.name_.trimmed().isEmpty()) {
    project.name_ = QFileInfo(qpath(project.dir_)).fileName();
    project.name_is_fallback_ = true;
  }
  project.last_scene_ = root.value(QStringLiteral("last_scene")).toString();
  // The whole manifest, kept as save()'s merge base — this is what makes a
  // rewrite by this build non-lossy for keys it does not model (v2, #325).
  project.raw_ = root;
  return project;
}

Expected<void> Project::save() {
  QJsonObject root = raw_;
  // Never downgrade: a manifest from a newer build keeps announcing itself,
  // while a v1 manifest becomes v2 the moment a v2 key could be written.
  version_ = std::max(version_, kSupportedVersion);
  root.insert(QStringLiteral("project_version"), version_);
  if (!name_is_fallback_) {
    root.insert(QStringLiteral("name"), name_);
  }
  if (last_scene_.isEmpty()) {
    root.remove(QStringLiteral("last_scene"));
  } else {
    root.insert(QStringLiteral("last_scene"), last_scene_);
  }

  const std::filesystem::path manifest = manifest_path(dir_);
  // QSaveFile, like create(): a half-written manifest must never mark a
  // directory as a broken project.
  QSaveFile file(qpath(manifest));
  if (!file.open(QIODevice::WriteOnly)) {
    return make_error(ErrorCode::IoFailure, "cannot write project.json", manifest.string());
  }
  file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
  if (!file.commit()) {
    return make_error(ErrorCode::IoFailure, "cannot write project.json", manifest.string());
  }
  // Keep the merge base in step so a second save() is byte-identical.
  raw_ = root;
  return {};
}

void Project::set_last_scene(const std::filesystem::path& scene) {
  const QString relative = QDir(qpath(dir_)).relativeFilePath(qpath(scene));
  // QDir happily answers "../elsewhere/x.xodr" for a scene outside the
  // project. A project may only point at its own scenes, and the manifest has
  // to stay portable — so an escape clears the field instead of storing it.
  if (relative.isEmpty() || relative.startsWith(QStringLiteral("..")) ||
      QDir::isAbsolutePath(relative)) {
    last_scene_.clear();
    return;
  }
  last_scene_ = relative;
}

std::optional<std::filesystem::path> Project::last_scene_path() const {
  if (last_scene_.isEmpty()) {
    return std::nullopt;
  }
  const std::filesystem::path candidate = dir_ / last_scene_.toStdString();
  std::error_code ec;
  if (!std::filesystem::is_regular_file(candidate, ec)) {
    return std::nullopt; // renamed, deleted, or never there — simply skip it
  }
  return candidate;
}

QStringList Project::scenes() const {
  // Top-level, non-recursive glob — subdirectories (assets/, autosaves, …)
  // are not scene locations. QDir::Name sorts case-insensitively on the
  // platforms whose file dialogs do, which is what the tiles should match.
  const QDir dir(qpath(dir_));
  QStringList result;
  const QFileInfoList entries =
      dir.entryInfoList({QStringLiteral("*.xodr")}, QDir::Files | QDir::Readable, QDir::Name);
  result.reserve(entries.size());
  for (const QFileInfo& entry : entries) {
    result.append(entry.absoluteFilePath());
  }
  return result;
}

std::filesystem::path Project::assets_dir() const {
  return dir_ / "assets";
}

std::optional<std::filesystem::path> Project::library_manifest_path() const {
  const std::filesystem::path candidate = assets_dir() / "library" / "manifest.json";
  std::error_code ec;
  if (std::filesystem::is_regular_file(candidate, ec)) {
    return candidate;
  }
  return std::nullopt;
}

std::optional<std::filesystem::path>
Project::find_project_for(const std::filesystem::path& scene_path) {
  const std::filesystem::path parent = scene_path.parent_path();
  if (parent.empty()) {
    return std::nullopt;
  }
  std::error_code ec;
  if (std::filesystem::is_regular_file(manifest_path(parent), ec)) {
    // Return the same normalized form open() stores, so callers can compare
    // "is this the project I already have open" with plain ==.
    const std::filesystem::path canonical = std::filesystem::weakly_canonical(parent, ec);
    return ec ? parent : canonical;
  }
  return std::nullopt;
}

} // namespace roadmaker::editor
