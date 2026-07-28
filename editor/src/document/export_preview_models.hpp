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

// Table models over a ScenePreview / XodrPreview (p7-s1, #241). Shaped like
// DiagnosticsModel: a thin QAbstractTableModel that reads a structure it does
// not own.
//
// The kernel hands over counts and metres as NUMBERS and a stable machine id
// per row; every piece of display text — translation, unit formatting, digit
// grouping — is decided here, so no formatted string ever crosses out of the
// editor into core.

#include "roadmaker/io/export_preview.hpp"

#include <QAbstractTableModel>
#include <QString>

namespace roadmaker::editor {

/// Human-readable name for a mesh channel. Translated here, never in the
/// kernel — `MeshChannelPreview::label` is the stable machine id.
[[nodiscard]] QString channel_display_name(MeshChannel channel);

/// A count with the locale's digit grouping ("12,480"). Counts are NOT
/// lengths and must never go through units::format_length.
[[nodiscard]] QString format_count(std::size_t value);

/// One row per NetworkMesh channel, exported or not.
class ExportChannelModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column : int {
    kChannel = 0,
    kElements,
    kExported,
    kTriangles,
    kVertices,
    kStatus,
    kColumnCount,
  };

  using QAbstractTableModel::QAbstractTableModel;

  /// Points the model at a preview it does not own. Pass nullptr to empty it.
  void set_preview(const ScenePreview* preview);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

private:
  const ScenePreview* preview_ = nullptr;
};

/// One row per material the exporter would write.
class ExportMaterialModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column : int { kName = 0, kColor, kRoughness, kTriangles, kColumnCount };

  using QAbstractTableModel::QAbstractTableModel;

  void set_preview(const ScenePreview* preview);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

private:
  const ScenePreview* preview_ = nullptr;
};

/// One row per `rm:` extension record type present in the written OpenDRIVE —
/// ADR-0008 Layer 1. Layer 2 (.rmscene.json) can never appear here: the rows
/// are counted out of the .xodr's own bytes.
class XodrRecordModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column : int { kCode = 0, kScope, kCount, kColumnCount };

  using QAbstractTableModel::QAbstractTableModel;

  void set_preview(const XodrPreview* preview);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

private:
  const XodrPreview* preview_ = nullptr;
};

} // namespace roadmaker::editor
