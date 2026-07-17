#include "document/library_manifest.hpp"

#include <spdlog/spdlog.h>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

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
  return LibraryItem::Kind::Unknown;
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
    manifest.items_.push_back(std::move(item));
  }
  return manifest;
}

Expected<LibraryManifest> LibraryManifest::load(const std::filesystem::path& path) {
  QFile file(QString::fromStdString(path.string()));
  if (!file.open(QIODevice::ReadOnly)) {
    return make_error(ErrorCode::IoFailure, "cannot read library manifest", path.string());
  }
  return parse(file.readAll());
}

} // namespace roadmaker::editor
