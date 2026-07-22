// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
  return create;
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
    item.model = create.value(QStringLiteral("model")).toString();
    item.signal = create.value(QStringLiteral("signal")).toString();
    item.mark_type = create.value(QStringLiteral("mark_type")).toString();
    item.mark_color = create.value(QStringLiteral("mark_color")).toString();
    item.mark_width = create.value(QStringLiteral("mark_width")).toDouble(0.12);
    item.material = create.value(QStringLiteral("material")).toString();
    item.crosswalk_width = create.value(QStringLiteral("width")).toDouble(3.0);
    item.crosswalk_border = create.value(QStringLiteral("border_width")).toDouble(0.0);
    item.crosswalk_dash = create.value(QStringLiteral("dash_length")).toDouble(0.5);
    item.crosswalk_gap = create.value(QStringLiteral("dash_gap")).toDouble(0.5);
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
  return manifest;
}

QByteArray LibraryManifest::to_json() const {
  QJsonArray items;
  for (const LibraryItem& item : items_) {
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
