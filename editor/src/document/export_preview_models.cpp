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

#include "document/export_preview_models.hpp"

#include <QColor>
#include <QLocale>

namespace roadmaker::editor {
namespace {

QString status_text(const MeshChannelPreview& row) {
  switch (row.reason) {
  case OmissionReason::None:
    return QObject::tr("Exported");
  case OmissionReason::ChannelEmpty:
    return QObject::tr("—");
  case OmissionReason::ChannelNotWalked:
    return QObject::tr("Not written");
  case OmissionReason::FormatUnsupported:
    return QObject::tr("Not supported by this format");
  case OmissionReason::ModelNotFound:
    return QObject::tr("Partly written");
  }
  return {};
}

/// Warning-coloured rows for anything the file will not carry. An empty
/// channel is not a warning — there is nothing to lose.
bool is_loss(const MeshChannelPreview& row) {
  return row.reason == OmissionReason::ChannelNotWalked ||
         row.reason == OmissionReason::FormatUnsupported ||
         row.reason == OmissionReason::ModelNotFound;
}

QString scope_text(RmCodeScope scope) {
  switch (scope) {
  case RmCodeScope::Road:
    return QObject::tr("road");
  case RmCodeScope::Object:
    return QObject::tr("object");
  case RmCodeScope::Junction:
    return QObject::tr("junction");
  case RmCodeScope::Root:
    return QObject::tr("root");
  }
  return {};
}

} // namespace

QString channel_display_name(MeshChannel channel) {
  switch (channel) {
  case MeshChannel::Roads:
    return QObject::tr("Roads");
  case MeshChannel::JunctionFloors:
    return QObject::tr("Junction floors");
  case MeshChannel::Surfaces:
    return QObject::tr("Ground surfaces");
  case MeshChannel::Terrain:
    return QObject::tr("Terrain");
  case MeshChannel::Bridges:
    return QObject::tr("Bridges");
  case MeshChannel::Objects:
    return QObject::tr("Props");
  case MeshChannel::SignalInstances:
    return QObject::tr("Signals and signs");
  case MeshChannel::SignalFaces:
    return QObject::tr("Sign face text");
  }
  return {};
}

QString format_count(std::size_t value) {
  return QLocale::system().toString(static_cast<qulonglong>(value));
}

// ------------------------------------------------------------- channels

void ExportChannelModel::set_preview(const ScenePreview* preview) {
  beginResetModel();
  preview_ = preview;
  endResetModel();
}

int ExportChannelModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() || preview_ == nullptr) {
    return 0;
  }
  return static_cast<int>(preview_->channels.size());
}

int ExportChannelModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant ExportChannelModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || preview_ == nullptr ||
      index.row() >= static_cast<int>(preview_->channels.size())) {
    return {};
  }
  const MeshChannelPreview& row = preview_->channels[static_cast<std::size_t>(index.row())];

  if (role == Qt::ToolTipRole) {
    return row.detail.empty() ? QVariant{} : QVariant(QString::fromStdString(row.detail));
  }
  if (role == Qt::ForegroundRole && is_loss(row)) {
    return QColor(0xD9, 0x7A, 0x1E);
  }
  if (role != Qt::DisplayRole) {
    return {};
  }
  switch (index.column()) {
  case kChannel:
    return channel_display_name(row.channel);
  case kElements:
    return format_count(row.elements);
  case kExported:
    return format_count(row.exported_elements);
  case kTriangles:
    return format_count(row.triangles);
  case kVertices:
    return format_count(row.vertices);
  case kStatus:
    return status_text(row);
  default:
    return {};
  }
}

QVariant
ExportChannelModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
    return {};
  }
  switch (section) {
  case kChannel:
    return tr("Channel");
  case kElements:
    return tr("In scene");
  case kExported:
    return tr("In file");
  case kTriangles:
    return tr("Triangles");
  case kVertices:
    return tr("Vertices");
  case kStatus:
    return tr("Status");
  default:
    return {};
  }
}

// ------------------------------------------------------------ materials

void ExportMaterialModel::set_preview(const ScenePreview* preview) {
  beginResetModel();
  preview_ = preview;
  endResetModel();
}

int ExportMaterialModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() || preview_ == nullptr) {
    return 0;
  }
  return static_cast<int>(preview_->materials.size());
}

int ExportMaterialModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant ExportMaterialModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || preview_ == nullptr ||
      index.row() >= static_cast<int>(preview_->materials.size())) {
    return {};
  }
  const MaterialPreview& material = preview_->materials[static_cast<std::size_t>(index.row())];

  if (role == Qt::DecorationRole && index.column() == kColor) {
    return QColor::fromRgbF(material.color[0], material.color[1], material.color[2],
                            material.color[3]);
  }
  if (role != Qt::DisplayRole) {
    return {};
  }
  switch (index.column()) {
  case kName:
    return QString::fromStdString(material.name);
  case kColor:
    return material.textured ? tr("textured") : QString{};
  case kRoughness:
    return QString::number(material.roughness, 'f', 2);
  case kTriangles:
    return format_count(material.triangles);
  default:
    return {};
  }
}

QVariant
ExportMaterialModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
    return {};
  }
  switch (section) {
  case kName:
    return tr("Material");
  case kColor:
    return tr("Colour");
  case kRoughness:
    return tr("Roughness");
  case kTriangles:
    return tr("Triangles");
  default:
    return {};
  }
}

// ----------------------------------------------------------- rm: records

void XodrRecordModel::set_preview(const XodrPreview* preview) {
  beginResetModel();
  preview_ = preview;
  endResetModel();
}

int XodrRecordModel::rowCount(const QModelIndex& parent) const {
  if (parent.isValid() || preview_ == nullptr) {
    return 0;
  }
  return static_cast<int>(preview_->rm_records.size());
}

int XodrRecordModel::columnCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : kColumnCount;
}

QVariant XodrRecordModel::data(const QModelIndex& index, int role) const {
  if (!index.isValid() || preview_ == nullptr ||
      index.row() >= static_cast<int>(preview_->rm_records.size())) {
    return {};
  }
  if (role != Qt::DisplayRole) {
    return {};
  }
  const XodrRecordPreview& record = preview_->rm_records[static_cast<std::size_t>(index.row())];
  switch (index.column()) {
  case kCode:
    return QString::fromStdString(record.code);
  case kScope:
    return scope_text(record.scope);
  case kCount:
    return format_count(record.count);
  default:
    return {};
  }
}

QVariant XodrRecordModel::headerData(int section, Qt::Orientation orientation, int role) const {
  if (role != Qt::DisplayRole || orientation != Qt::Horizontal) {
    return {};
  }
  switch (section) {
  case kCode:
    return tr("Extension");
  case kScope:
    return tr("Scope");
  case kCount:
    return tr("Records");
  default:
    return {};
  }
}

} // namespace roadmaker::editor
