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

#include "document/asset_import.hpp"

#include "roadmaker/assets/prop_import.hpp"
#include "roadmaker/assets/prop_library.hpp"

#include <fmt/format.h>

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <array>
#include <tuple>
#include <utility>
#include <vector>

namespace roadmaker::editor {

namespace {

/// The image extensions this accepts. Q7 of the realignment guarantees PNG and
/// JPEG; the rest are whatever Qt was built with, and are listed because a user
/// who has a .webp texture should not be told it is unsupported when it is not.
const QStringList& image_extensions() {
  static const QStringList kExtensions = {QStringLiteral("png"),
                                          QStringLiteral("jpg"),
                                          QStringLiteral("jpeg"),
                                          QStringLiteral("bmp"),
                                          QStringLiteral("webp"),
                                          QStringLiteral("tif"),
                                          QStringLiteral("tiff")};
  return kExtensions;
}

/// Thumbnail edge, matching the committed bundled thumbnails
/// (scripts/gen_library_thumbnails.py SIZE = 96) so a project asset's row is the
/// same size as a built-in one's.
constexpr int kThumbnailSize = 96;

/// Where inside the project each kind's bytes live.
std::filesystem::path asset_subdir(AssetImportKind kind) {
  switch (kind) {
  case AssetImportKind::Material:
    return "textures";
  case AssetImportKind::Prop:
    return "props";
  }
  return "textures";
}

/// Writes a scaled thumbnail. Decodes AT the target size via QImageReader rather
/// than loading and then shrinking — the trick project_files_model.cpp already
/// uses so a 4K texture never costs a full-resolution QImage for a 96 px tile.
Expected<void> write_thumbnail(const std::filesystem::path& source,
                               const std::filesystem::path& destination) {
  QImageReader reader(QString::fromStdString(source.string()));
  reader.setAutoTransform(true);
  const QSize size = reader.size();
  if (size.isValid() && !size.isEmpty()) {
    const QSize scaled = size.scaled(kThumbnailSize, kThumbnailSize, Qt::KeepAspectRatio);
    reader.setScaledSize(scaled.isEmpty() ? QSize(kThumbnailSize, kThumbnailSize) : scaled);
  }
  const QImage image = reader.read();
  if (image.isNull()) {
    return make_error(ErrorCode::InvalidDocument,
                      "image could not be decoded: " + reader.errorString().toStdString(),
                      source.string());
  }
  std::error_code ec;
  std::filesystem::create_directories(destination.parent_path(), ec);
  // QSaveFile so a half-written thumbnail never appears — the same atomicity the
  // manifest and project.json are written with.
  QSaveFile file(QString::fromStdString(destination.string()));
  if (!file.open(QIODevice::WriteOnly)) {
    return make_error(
        ErrorCode::IoFailure, "cannot open thumbnail for writing", destination.string());
  }
  if (!image.save(&file, "PNG")) {
    return make_error(ErrorCode::IoFailure, "cannot encode thumbnail", destination.string());
  }
  if (!file.commit()) {
    return make_error(ErrorCode::IoFailure, "cannot commit thumbnail", destination.string());
  }
  return {};
}

/// A slug not already taken by a file in `dir` with the same extension, nor by
/// `taken`. Returns {slug, renamed}.
std::pair<QString, bool>
unique_slug(const QString& wanted, const std::filesystem::path& dir, const QString& extension) {
  const auto occupied = [&](const QString& candidate) {
    std::error_code ec;
    return std::filesystem::exists(
        dir / (candidate + QStringLiteral(".") + extension).toStdString(), ec);
  };
  if (!occupied(wanted)) {
    return {wanted, false};
  }
  for (int n = 2; n < 1000; ++n) {
    const QString candidate = wanted + QStringLiteral("_%1").arg(n);
    if (!occupied(candidate)) {
      return {candidate, true};
    }
  }
  return {wanted, false};
}

/// Project-relative, forward-slashed, which is how the manifest stores a path so
/// it stays portable across platforms.
QString manifest_relative(const std::filesystem::path& project_dir,
                          const std::filesystem::path& path) {
  const std::filesystem::path relative = path.lexically_relative(project_dir);
  return QDir::fromNativeSeparators(QString::fromStdString(relative.string()));
}

} // namespace

std::optional<AssetImportKind> asset_import_kind(const std::filesystem::path& path) {
  // Models first: the model reader owns its own closed extension list (ADR-0013),
  // so this cannot drift from what it actually accepts.
  if (props::is_prop_model_extension(path)) {
    return AssetImportKind::Prop;
  }
  const QString extension = QFileInfo(QString::fromStdString(path.string())).suffix().toLower();
  if (image_extensions().contains(extension)) {
    return AssetImportKind::Material;
  }
  return std::nullopt;
}

QString asset_import_filter() {
  QStringList images;
  for (const QString& extension : image_extensions()) {
    images.push_back(QStringLiteral("*.") + extension);
  }
  const QString models = QStringLiteral("*.glb *.gltf");
  return QStringLiteral("Assets (%1 %2);;Images (%1);;Models (%2)")
      .arg(images.join(QLatin1Char(' ')), models);
}

QString asset_slug(const QString& label) {
  QString slug;
  slug.reserve(label.size());
  bool pending_separator = false;
  for (const QChar character : label) {
    if (character.isLetterOrNumber()) {
      // ASCII-fold nothing: a non-ASCII letter is kept, because a slug is a
      // filename and both Qt and std::filesystem handle UTF-8 paths. Only
      // separators and punctuation are normalised.
      if (pending_separator && !slug.isEmpty()) {
        slug.append(QLatin1Char('_'));
      }
      slug.append(character.toLower());
      pending_separator = false;
    } else {
      pending_separator = true;
    }
  }
  return slug;
}

Expected<std::pair<LibraryMaterial, AssetImportResult>>
import_material_asset(const std::filesystem::path& project_dir, const AssetImportRequest& request) {
  const std::optional<AssetImportKind> kind = asset_import_kind(request.source);
  if (kind != AssetImportKind::Material) {
    return make_error(ErrorCode::InvalidArgument,
                      "that file is not an image RoadMaker imports as a material",
                      request.source.string());
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(request.source, ec)) {
    return make_error(ErrorCode::FileNotFound, "the file does not exist", request.source.string());
  }
  const QString wanted = asset_slug(request.label);
  if (wanted.isEmpty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "the asset needs a name with at least one letter or digit");
  }

  const std::filesystem::path textures = project_dir / "assets" / asset_subdir(*kind);
  std::filesystem::create_directories(textures, ec);
  if (!std::filesystem::is_directory(textures, ec)) {
    return make_error(ErrorCode::IoFailure,
                      "could not create the project's asset folder: " + ec.message(),
                      textures.string());
  }
  const QString extension =
      QFileInfo(QString::fromStdString(request.source.string())).suffix().toLower();
  const auto [slug, renamed] = unique_slug(wanted, textures, extension);

  const std::filesystem::path copied =
      textures / (slug + QStringLiteral(".") + extension).toStdString();
  // copy_file without overwrite_existing: unique_slug already proved the path is
  // free, and asking for the overwrite would defeat the point of having checked.
  std::filesystem::copy_file(request.source, copied, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure,
                      "could not copy the file into the project: " + ec.message(),
                      request.source.string());
  }

  const std::filesystem::path thumbnail = project_dir / "assets" / "library" / "thumbnails" /
                                          (slug + QStringLiteral(".png")).toStdString();
  if (Expected<void> written = write_thumbnail(copied, thumbnail); !written.has_value()) {
    // The copy is already on disk. Leaving it there with no manifest entry would
    // be litter the user cannot see, so undo it before reporting.
    std::error_code cleanup;
    std::filesystem::remove(copied, cleanup);
    return make_error(written.error().code, written.error().message, written.error().context);
  }

  LibraryMaterial material;
  // `rm:` namespaced, because this id is what a .xodr will store in
  // <material surface> and it has to be recognisable as ours in a foreign file.
  material.id = QStringLiteral("rm:") + slug;
  material.label = request.label;
  material.category = request.category.isEmpty() ? QStringLiteral("Materials") : request.category;
  material.thumbnail = manifest_relative(project_dir, thumbnail);
  material.albedo = manifest_relative(project_dir, copied);
  // No normal or roughness map: one image is one albedo. The scalar params below
  // are what the renderer falls back to, and they are the compiled-in defaults so
  // an imported material reads like a plain painted surface rather than a guess.
  material.source = QString::fromStdString(request.source.string());
  material.license = request.license;

  AssetImportResult result;
  result.key = QStringLiteral("material.") + slug;
  result.slug = slug;
  result.copied_to = copied;
  result.renamed = renamed;
  return std::pair{std::move(material), std::move(result)};
}

Expected<std::tuple<LibraryItem, AssetImportResult, std::vector<Diagnostic>>>
import_prop_asset(const std::filesystem::path& project_dir, const AssetImportRequest& request) {
  const std::optional<AssetImportKind> kind = asset_import_kind(request.source);
  if (kind != AssetImportKind::Prop) {
    return make_error(ErrorCode::InvalidArgument,
                      "that file is not a model RoadMaker imports as a prop",
                      request.source.string());
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(request.source, ec)) {
    return make_error(ErrorCode::FileNotFound, "the file does not exist", request.source.string());
  }
  const QString wanted = asset_slug(request.label);
  if (wanted.isEmpty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "the asset needs a name with at least one letter or digit");
  }

  // ★ READ IT BEFORE COPYING IT. A model that cannot be turned into a prop must
  // not leave bytes inside the project — and this is also where the budgets, the
  // frame conversion and the flatten warning happen, so the user hears about all
  // of it before anything is committed.
  auto probe = props::import_prop_model(request.source, wanted.toStdString());
  if (!probe.has_value()) {
    return make_error(probe.error().code, probe.error().message, probe.error().context);
  }

  const std::filesystem::path models = project_dir / "assets" / asset_subdir(*kind);
  std::filesystem::create_directories(models, ec);
  if (!std::filesystem::is_directory(models, ec)) {
    return make_error(ErrorCode::IoFailure,
                      "could not create the project's asset folder: " + ec.message(),
                      models.string());
  }
  const QString extension =
      QFileInfo(QString::fromStdString(request.source.string())).suffix().toLower();
  const auto [slug, renamed] = unique_slug(wanted, models, extension);

  const std::filesystem::path copied =
      models / (slug + QStringLiteral(".") + extension).toStdString();
  std::filesystem::copy_file(request.source, copied, ec);
  if (ec) {
    return make_error(ErrorCode::IoFailure,
                      "could not copy the model into the project: " + ec.message(),
                      request.source.string());
  }

  LibraryItem item;
  item.key = QStringLiteral("prop.") + slug;
  item.label = request.label;
  item.category = request.category.isEmpty() ? QStringLiteral("Props") : request.category;
  // `kind: tree` is the create-intent tag for EVERY point prop; the OpenDRIVE
  // class comes from props::model(id)->type, not from this tag.
  item.kind = LibraryItem::Kind::Tree;
  item.model = slug;
  // No thumbnail: #322 puts render-to-thumbnail out of scope (#509) and the panel
  // already falls back to a themed glyph for an item without one.
  //
  // create_raw carries the fields LibraryItem does not model — the model file and
  // the provenance pair — because to_json re-emits it verbatim. That is the same
  // mechanism #336 used to persist default_scale.
  QJsonObject create;
  create[QStringLiteral("kind")] = QStringLiteral("tree");
  create[QStringLiteral("model")] = slug;
  create[QStringLiteral("model_file")] = manifest_relative(project_dir, copied);
  if (!request.source.empty()) {
    create[QStringLiteral("source")] = QString::fromStdString(request.source.string());
  }
  if (!request.license.isEmpty()) {
    create[QStringLiteral("license")] = request.license;
  }
  item.create_raw = create;

  AssetImportResult result;
  result.key = item.key;
  result.slug = slug;
  result.copied_to = copied;
  result.renamed = renamed;
  return std::tuple{std::move(item), std::move(result), std::move(probe->diagnostics)};
}

std::vector<Diagnostic> register_project_prop_models(const std::filesystem::path& project_dir,
                                                     const LibraryManifest& manifest) {
  std::vector<Diagnostic> diagnostics;
  std::vector<props::PropModel> models;
  for (const LibraryItem& item : manifest.items()) {
    if (item.kind != LibraryItem::Kind::Tree) {
      continue;
    }
    const QString file = item.create_raw.value(QStringLiteral("model_file")).toString();
    if (file.isEmpty()) {
      continue; // a bundled model, resolved from the compiled-in catalogue
    }
    const std::filesystem::path path(file.toStdString());
    const std::filesystem::path absolute = path.is_absolute() ? path : project_dir / path;
    auto imported = props::import_prop_model(absolute, item.model.toStdString());
    if (!imported.has_value()) {
      // The project still opens. A missing or broken asset is a diagnostic, not a
      // reason to refuse the whole library.
      diagnostics.push_back(
          Diagnostic{.severity = Severity::Warning,
                     .location = file.toStdString(),
                     .message = fmt::format("prop asset \"{}\" could not be loaded and will not "
                                            "draw: {}",
                                            item.model.toStdString(),
                                            imported.error().message)});
      continue;
    }
    // The reader's own diagnostics (a flattened texture, a skipped primitive) are
    // forwarded on every project open, not only at import time — otherwise a user
    // who reopens a project never learns why their chair is flat.
    for (Diagnostic& diagnostic : imported->diagnostics) {
      if (diagnostic.severity != Severity::Info) {
        diagnostics.push_back(std::move(diagnostic));
      }
    }
    models.push_back(std::move(imported->model));
  }
  props::register_project_models(std::move(models));
  return diagnostics;
}

} // namespace roadmaker::editor
