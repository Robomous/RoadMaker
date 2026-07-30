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

// The one import in RoadMaker that gets a dialog (p6-s8, #322).
//
// WHY THIS DEVIATES FROM THE HOUSE STYLE. Every other import — GIS vector and
// raster, point clouds, DEMs, OSM extracts — is deliberately a bare QFileDialog
// with no options step, and main_window.cpp says why: "there is nothing to
// choose, and an import does not need one". That reasoning does not survive here.
// #322's acceptance requires a licence note recorded per import, and an
// attestation is by definition something only the user can supply — it cannot be
// derived from the file the way a CRS or a bounding box can. The name and category
// ride along because the user is already being asked something.
//
// Thin by design: it collects three strings and a confirmation, and hands them to
// the headless `asset_import` functions. No importing happens here.

#include <QDialog>
#include <QString>
#include <QStringList>
#include <filesystem>

#include "document/asset_import.hpp"

class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;

namespace roadmaker::editor {

class AssetImportDialog : public QDialog {
  Q_OBJECT

public:
  /// `categories` seeds the category combo with what the merged library already
  /// uses, so an import lands in an existing group rather than inventing one.
  AssetImportDialog(const std::filesystem::path& source,
                    const QStringList& categories,
                    QWidget* parent = nullptr);

  /// What the user chose. Only meaningful after `exec()` returned Accepted.
  [[nodiscard]] AssetImportRequest request() const;

private:
  void update_ok_enabled();

  std::filesystem::path source_;
  QLineEdit* name_edit_ = nullptr;
  QComboBox* category_combo_ = nullptr;
  QLineEdit* license_edit_ = nullptr;
  QCheckBox* attestation_ = nullptr;
  QPushButton* ok_button_ = nullptr;
};

} // namespace roadmaker::editor
