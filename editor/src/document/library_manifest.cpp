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

#include "document/library_manifest.hpp"

#include "roadmaker/assets/prop_library.hpp"

#include <spdlog/spdlog.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <algorithm>
#include <optional>

namespace roadmaker::editor {

namespace {

LibraryItem::Kind parse_kind(const QString& kind) {
  if (kind == QStringLiteral("road_template")) {
    return LibraryItem::Kind::RoadTemplate;
  }
  if (kind == QStringLiteral("road_style")) {
    return LibraryItem::Kind::RoadStyle;
  }
  if (kind == QStringLiteral("assembly")) {
    return LibraryItem::Kind::Assembly;
  }
  if (kind == QStringLiteral("tree")) {
    return LibraryItem::Kind::Tree;
  }
  if (kind == QStringLiteral("signal")) {
    return LibraryItem::Kind::Signal;
  }
  if (kind == QStringLiteral("marking")) {
    return LibraryItem::Kind::Marking;
  }
  if (kind == QStringLiteral("material")) {
    return LibraryItem::Kind::Material;
  }
  if (kind == QStringLiteral("crosswalk")) {
    return LibraryItem::Kind::Crosswalk;
  }
  if (kind == QStringLiteral("stencil")) {
    return LibraryItem::Kind::Stencil;
  }
  if (kind == QStringLiteral("prop_set")) {
    return LibraryItem::Kind::PropSet;
  }
  // ★ NOT "assembly" — that kind is the T/X road-junction template and has been
  // since it shipped. A composite prop takes its own kind, which costs nothing
  // because an unrecognised kind already parses to Unknown and round-trips
  // verbatim through create_raw (p6-s9, #323).
  if (kind == QStringLiteral("prop_assembly")) {
    return LibraryItem::Kind::PropAssembly;
  }
  return LibraryItem::Kind::Unknown;
}

/// The `create` JSON block for one item. A parsed item re-emits its verbatim
/// create_raw (round-trips unknown kinds + forward-compat fields); a
/// programmatically built crosswalk item is serialized from its fields.
QJsonObject create_object(const LibraryItem& item) {
  if (!item.create_raw.isEmpty()) {
    return item.create_raw;
  }
  QJsonObject create;
  if (item.kind == LibraryItem::Kind::Crosswalk) {
    create[QStringLiteral("kind")] = QStringLiteral("crosswalk");
    create[QStringLiteral("width")] = item.crosswalk_width;
    create[QStringLiteral("border_width")] = item.crosswalk_border;
    create[QStringLiteral("dash_length")] = item.crosswalk_dash;
    create[QStringLiteral("dash_gap")] = item.crosswalk_gap;
    if (!item.crosswalk_material.isEmpty()) {
      create[QStringLiteral("material")] = item.crosswalk_material;
    }
    if (!item.crosswalk_segmentation.isEmpty()) {
      create[QStringLiteral("segmentation")] = item.crosswalk_segmentation;
    }
  }
  if (item.kind == LibraryItem::Kind::Stencil) {
    create[QStringLiteral("kind")] = QStringLiteral("stencil");
    create[QStringLiteral("subtype")] = item.stencil_subtype;
    create[QStringLiteral("length")] = item.stencil_length;
    create[QStringLiteral("width_frac")] = item.stencil_width_frac;
    if (!item.stencil_material.isEmpty()) {
      create[QStringLiteral("material")] = item.stencil_material;
    }
    if (!item.stencil_segmentation.isEmpty()) {
      create[QStringLiteral("segmentation")] = item.stencil_segmentation;
    }
  }
  if (item.kind == LibraryItem::Kind::PropSet) {
    create[QStringLiteral("kind")] = QStringLiteral("prop_set");
    QJsonArray entries;
    for (const LibraryItem::PropSetEntry& entry : item.prop_entries) {
      QJsonObject object;
      object[QStringLiteral("model")] = entry.model;
      object[QStringLiteral("portion")] = entry.portion;
      entries.push_back(object);
    }
    create[QStringLiteral("entries")] = entries;
  }
  if (item.kind == LibraryItem::Kind::Material) {
    // Was missing entirely before p6-s8: a programmatically built material item
    // with no create_raw serialized an EMPTY create block, so it round-tripped
    // as an unusable row.
    create[QStringLiteral("kind")] = QStringLiteral("material");
    create[QStringLiteral("material")] = item.material;
  }
  if (item.kind == LibraryItem::Kind::PropAssembly) {
    create[QStringLiteral("kind")] = QStringLiteral("prop_assembly");
    create[QStringLiteral("prop_assembly")] = item.prop_assembly;
  }
  if (item.kind == LibraryItem::Kind::Tree) {
    create[QStringLiteral("kind")] = QStringLiteral("tree");
    create[QStringLiteral("model")] = item.model;
    // Default 1.0 stays implicit so a native-size prop item is byte-identical
    // to one authored without the field.
    if (item.default_scale != 1.0) {
      create[QStringLiteral("default_scale")] = item.default_scale;
    }
  }
  return create;
}

/// Reads one `materials[]` entry (p6-s8, #322). The schema is
/// docs/design/materials-structures/01_material_system.md §2; anything this build
/// does not model survives in `raw`.
std::optional<LibraryMaterial> parse_material(const QJsonObject& object) {
  LibraryMaterial material;
  material.id = object.value(QStringLiteral("id")).toString();
  if (material.id.isEmpty()) {
    spdlog::warn("library manifest: skipping a material without an id");
    return std::nullopt;
  }
  material.label = object.value(QStringLiteral("label")).toString(material.id);
  material.category = object.value(QStringLiteral("category")).toString();
  material.thumbnail = object.value(QStringLiteral("thumbnail")).toString();

  const QJsonObject maps = object.value(QStringLiteral("maps")).toObject();
  material.albedo = maps.value(QStringLiteral("albedo")).toString();
  material.normal = maps.value(QStringLiteral("normal")).toString();
  material.roughness = maps.value(QStringLiteral("roughness")).toString();

  const QJsonValue uv_scale = object.value(QStringLiteral("uv_scale"));
  if (uv_scale.isDouble() && uv_scale.toDouble() > 0.0) {
    material.uv_scale = uv_scale.toDouble();
  } else if (!uv_scale.isUndefined()) {
    // A non-positive uv_scale would divide the texture by zero metres. Coerce and
    // say so rather than rendering a solid smear.
    spdlog::warn("library manifest: material {} has a non-positive uv_scale; using {}",
                 material.id.toStdString(),
                 material.uv_scale);
  }

  const QJsonObject params = object.value(QStringLiteral("params")).toObject();
  if (params.value(QStringLiteral("roughness")).isDouble()) {
    material.param_roughness = params.value(QStringLiteral("roughness")).toDouble();
  }
  if (params.value(QStringLiteral("normal_strength")).isDouble()) {
    material.normal_strength = params.value(QStringLiteral("normal_strength")).toDouble();
  }
  if (params.value(QStringLiteral("friction")).isDouble()) {
    material.friction = params.value(QStringLiteral("friction")).toDouble();
  }
  const QJsonArray tint = params.value(QStringLiteral("tint")).toArray();
  if (tint.size() == 4) {
    for (int channel = 0; channel < 4; ++channel) {
      material.tint[static_cast<std::size_t>(channel)] = tint.at(channel).toDouble(1.0);
    }
  } else if (!tint.isEmpty()) {
    spdlog::warn("library manifest: material {} has a tint that is not 4 channels; ignoring it",
                 material.id.toStdString());
  }

  material.source = object.value(QStringLiteral("source")).toString();
  material.license = object.value(QStringLiteral("license")).toString();
  material.raw = object;
  return material;
}

QJsonObject material_object(const LibraryMaterial& material) {
  // Start from the verbatim entry so forward-compat fields round-trip, then
  // overwrite what this build models — the same contract create_raw carries.
  QJsonObject object = material.raw;
  object[QStringLiteral("id")] = material.id;
  object[QStringLiteral("label")] = material.label;
  if (!material.category.isEmpty()) {
    object[QStringLiteral("category")] = material.category;
  }
  if (!material.thumbnail.isEmpty()) {
    object[QStringLiteral("thumbnail")] = material.thumbnail;
  }
  QJsonObject maps;
  if (!material.albedo.isEmpty()) {
    maps[QStringLiteral("albedo")] = material.albedo;
  }
  if (!material.normal.isEmpty()) {
    maps[QStringLiteral("normal")] = material.normal;
  }
  if (!material.roughness.isEmpty()) {
    maps[QStringLiteral("roughness")] = material.roughness;
  }
  if (maps.isEmpty()) {
    object.remove(QStringLiteral("maps"));
  } else {
    object[QStringLiteral("maps")] = maps;
  }
  object[QStringLiteral("uv_scale")] = material.uv_scale;
  QJsonObject params;
  params[QStringLiteral("roughness")] = material.param_roughness;
  params[QStringLiteral("normal_strength")] = material.normal_strength;
  params[QStringLiteral("friction")] = material.friction;
  QJsonArray tint;
  for (const double channel : material.tint) {
    tint.push_back(channel);
  }
  params[QStringLiteral("tint")] = tint;
  object[QStringLiteral("params")] = params;
  if (material.source.isEmpty()) {
    object.remove(QStringLiteral("source"));
  } else {
    object[QStringLiteral("source")] = material.source;
  }
  if (material.license.isEmpty()) {
    object.remove(QStringLiteral("license"));
  } else {
    object[QStringLiteral("license")] = material.license;
  }
  return object;
}

/// The catalogue key a material definition is presented under. Matches the
/// spelling the five bundled materials already use, so a project material and a
/// bundled one are indistinguishable to the panel and the drop handler.
QString material_item_key(const QString& id) {
  QString bare = id;
  if (bare.startsWith(QStringLiteral("rm:"))) {
    bare.remove(0, 3);
  }
  if (bare.startsWith(QStringLiteral("material."))) {
    return bare;
  }
  return QStringLiteral("material.") + bare;
}

} // namespace

Expected<LibraryManifest> LibraryManifest::parse(const QByteArray& json) {
  QJsonParseError error{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &error);
  if (error.error != QJsonParseError::NoError) {
    return make_error(ErrorCode::InvalidArgument,
                      "library manifest is not valid JSON: " + error.errorString().toStdString());
  }
  if (!document.isObject()) {
    return make_error(ErrorCode::InvalidArgument, "library manifest root is not an object");
  }
  const QJsonObject root = document.object();

  const QJsonValue version = root.value(QStringLiteral("manifest_version"));
  if (!version.isDouble()) {
    return make_error(ErrorCode::InvalidArgument,
                      "library manifest is missing an integer manifest_version");
  }
  if (!root.value(QStringLiteral("items")).isArray()) {
    return make_error(ErrorCode::InvalidArgument, "library manifest is missing an items array");
  }

  LibraryManifest manifest;
  manifest.version_ = version.toInt();
  // Forward compatibility: a newer schema still parses the fields this build
  // knows; only warn so a future manifest never bricks an older editor.
  if (manifest.version_ > kSupportedVersion) {
    spdlog::warn("library manifest version {} is newer than supported version {} — "
                 "parsing known fields only",
                 manifest.version_,
                 kSupportedVersion);
  }

  for (const QJsonValue& entry : root.value(QStringLiteral("items")).toArray()) {
    const QJsonObject object = entry.toObject();
    const QString key = object.value(QStringLiteral("key")).toString();
    if (key.isEmpty()) {
      spdlog::warn("library manifest: skipping an item without a key");
      continue;
    }
    const QJsonObject create = object.value(QStringLiteral("create")).toObject();
    LibraryItem item;
    item.key = key;
    item.label = object.value(QStringLiteral("label")).toString(key);
    item.category = object.value(QStringLiteral("category")).toString();
    item.thumbnail = object.value(QStringLiteral("thumbnail")).toString();
    item.kind = parse_kind(create.value(QStringLiteral("kind")).toString());
    item.profile = create.value(QStringLiteral("profile")).toString();
    item.style = create.value(QStringLiteral("style")).toString();
    item.assembly = create.value(QStringLiteral("assembly")).toString();
    item.prop_assembly = create.value(QStringLiteral("prop_assembly")).toString();
    item.model = create.value(QStringLiteral("model")).toString();
    item.default_scale = create.value(QStringLiteral("default_scale")).toDouble(1.0);
    if (!(item.default_scale > 0.0)) {
      spdlog::warn("library manifest: item '{}' has a non-positive default_scale — using 1.0",
                   key.toStdString());
      item.default_scale = 1.0;
    }
    item.signal = create.value(QStringLiteral("signal")).toString();
    item.mark_type = create.value(QStringLiteral("mark_type")).toString();
    item.mark_color = create.value(QStringLiteral("mark_color")).toString();
    item.mark_width =
        create.value(QStringLiteral("mark_width")).toDouble(roadmaker::defaults::kLineWidth);
    item.material = create.value(QStringLiteral("material")).toString();
    item.crosswalk_width =
        create.value(QStringLiteral("width")).toDouble(roadmaker::defaults::kCrosswalkWidth);
    item.crosswalk_border = create.value(QStringLiteral("border_width")).toDouble(0.0);
    item.crosswalk_dash = create.value(QStringLiteral("dash_length"))
                              .toDouble(roadmaker::defaults::kCrosswalkStripeLength);
    item.crosswalk_gap =
        create.value(QStringLiteral("dash_gap")).toDouble(roadmaker::defaults::kCrosswalkStripeGap);
    item.crosswalk_material = create.value(QStringLiteral("material")).toString();
    item.crosswalk_segmentation = create.value(QStringLiteral("segmentation")).toString();
    item.stencil_subtype = create.value(QStringLiteral("subtype")).toString();
    item.stencil_length = create.value(QStringLiteral("length")).toDouble(4.0);
    item.stencil_width_frac = create.value(QStringLiteral("width_frac")).toDouble(0.5);
    item.stencil_material = create.value(QStringLiteral("material")).toString();
    item.stencil_segmentation = create.value(QStringLiteral("segmentation")).toString();
    // PropSet entries: drop any whose model doesn't resolve to a bundled prop or
    // whose portion is not positive (a zero/negative weight can never be drawn).
    // The verbatim create_raw below still round-trips the authored array intact.
    for (const QJsonValue& entry_value : create.value(QStringLiteral("entries")).toArray()) {
      const QJsonObject entry_object = entry_value.toObject();
      const QString model = entry_object.value(QStringLiteral("model")).toString();
      const double portion = entry_object.value(QStringLiteral("portion")).toDouble(1.0);
      if (portion <= 0.0 || props::model(model.toStdString()) == nullptr) {
        continue;
      }
      item.prop_entries.push_back(LibraryItem::PropSetEntry{.model = model, .portion = portion});
    }
    // Capture the verbatim create block so unknown kinds and forward-compat
    // fields survive to_json() untouched (the never-drop contract for the
    // manifest schema).
    item.create_raw = create;
    manifest.items_.push_back(std::move(item));
  }

  // materials[] (v2, p6-s8 #322). Absent in a v1 manifest, which is exactly what
  // "a v1 manifest parses fine, it just has no project materials" means.
  for (const QJsonValue& entry : root.value(QStringLiteral("materials")).toArray()) {
    if (std::optional<LibraryMaterial> material = parse_material(entry.toObject())) {
      manifest.materials_.push_back(std::move(*material));
    }
  }
  manifest.resync_material_rows();
  return manifest;
}

void LibraryManifest::resync_material_rows() {
  // Drop the rows this build invented last time, then re-invent them. Rows the
  // manifest actually declared in items[] are left alone.
  items_.erase(std::remove_if(items_.begin(),
                              items_.end(),
                              [](const LibraryItem& item) { return item.synthesized; }),
               items_.end());
  for (const LibraryMaterial& material : materials_) {
    const QString key = material_item_key(material.id);
    // An explicit items[] row wins: a manifest that spells the row out gets what
    // it asked for, and nothing is presented twice.
    const bool declared = std::any_of(
        items_.begin(), items_.end(), [&key](const LibraryItem& i) { return i.key == key; });
    if (declared) {
      continue;
    }
    LibraryItem row;
    row.key = key;
    row.label = material.label;
    row.category = material.category.isEmpty() ? QStringLiteral("Materials") : material.category;
    row.thumbnail = material.thumbnail;
    row.kind = LibraryItem::Kind::Material;
    row.material = material.id;
    row.synthesized = true;
    items_.push_back(std::move(row));
  }
}

QByteArray LibraryManifest::to_json() const {
  QJsonArray items;
  for (const LibraryItem& item : items_) {
    // A synthesized row is a VIEW onto a materials[] entry, not a record of its
    // own. Writing it back would duplicate the definition into items[] and let
    // the two copies drift.
    if (item.synthesized) {
      continue;
    }
    QJsonObject object;
    object[QStringLiteral("key")] = item.key;
    if (!item.label.isEmpty()) {
      object[QStringLiteral("label")] = item.label;
    }
    if (!item.category.isEmpty()) {
      object[QStringLiteral("category")] = item.category;
    }
    if (!item.thumbnail.isEmpty()) {
      object[QStringLiteral("thumbnail")] = item.thumbnail;
    }
    object[QStringLiteral("create")] = create_object(item);
    items.push_back(object);
  }
  QJsonObject root;
  root[QStringLiteral("manifest_version")] = version_;
  root[QStringLiteral("items")] = items;
  if (!materials_.empty()) {
    // Omitted entirely when there are none, so a project that defines no
    // materials writes the same bytes a v1 build would have.
    QJsonArray materials;
    for (const LibraryMaterial& material : materials_) {
      materials.push_back(material_object(material));
    }
    root[QStringLiteral("materials")] = materials;
  }
  return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

Expected<void> LibraryManifest::save(const std::filesystem::path& path) const {
  // QSaveFile: the manifest appears atomically or not at all (Project::create
  // pattern) — a half-written overlay would break the next load.
  QSaveFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::WriteOnly)) {
    return make_error(
        ErrorCode::IoFailure, "cannot open library manifest for writing", path.string());
  }
  file.write(to_json());
  if (!file.commit()) {
    return make_error(ErrorCode::IoFailure, "cannot commit library manifest", path.string());
  }
  return {};
}

void LibraryManifest::upsert(LibraryItem item) {
  for (LibraryItem& existing : items_) {
    if (existing.key == item.key) {
      existing = std::move(item);
      return;
    }
  }
  items_.push_back(std::move(item));
}

void LibraryManifest::upsert_material(LibraryMaterial material) {
  for (LibraryMaterial& existing : materials_) {
    if (existing.id == material.id) {
      existing = std::move(material);
      resync_material_rows();
      return;
    }
  }
  materials_.push_back(std::move(material));
  resync_material_rows();
}

bool LibraryManifest::remove_material(const QString& id) {
  const auto it = std::find_if(
      materials_.begin(), materials_.end(), [&id](const LibraryMaterial& m) { return m.id == id; });
  if (it == materials_.end()) {
    return false;
  }
  materials_.erase(it);
  resync_material_rows();
  return true;
}

bool LibraryManifest::remove(const QString& key) {
  const auto it = std::find_if(
      items_.begin(), items_.end(), [&](const LibraryItem& item) { return item.key == key; });
  if (it == items_.end()) {
    return false;
  }
  items_.erase(it);
  return true;
}

Expected<LibraryManifest> LibraryManifest::load(const std::filesystem::path& path) {
  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return make_error(ErrorCode::IoFailure, "cannot read library manifest", path.string());
  }
  return parse(file.readAll());
}

} // namespace roadmaker::editor
