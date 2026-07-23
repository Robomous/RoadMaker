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

// Table model over the parser/validator diagnostics: severity | rule id |
// location | message. Rule id and entity ids come straight from the kernel
// Diagnostic (empty rule id = no normative ASAM rule applies).

#include <QAbstractTableModel>

#include "document/document.hpp"

namespace roadmaker::editor {

class DiagnosticsModel : public QAbstractTableModel {
  Q_OBJECT

public:
  enum Column : int { kSeverity = 0, kRuleId, kLocation, kMessage, kColumnCount };

  explicit DiagnosticsModel(const Document& document, QObject* parent = nullptr);

  [[nodiscard]] int rowCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] int columnCount(const QModelIndex& parent = {}) const override;
  [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
  [[nodiscard]] QVariant
  headerData(int section, Qt::Orientation orientation, int role) const override;

  /// The diagnostic behind a row (nullptr when out of range) — used by the
  /// panel to resolve double-clicks into a selection.
  [[nodiscard]] const Diagnostic* diagnostic_at(int row) const;

private:
  const Document& document_;
};

} // namespace roadmaker::editor
