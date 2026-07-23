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

// Thin QTableView host over DiagnosticsModel. Double-clicking a row resolves
// its location string to an entity (best effort) and routes the result
// through SelectionModel.

#include <QTableView>
#include <QWidget>

#include "document/diagnostics_model.hpp"
#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

class DiagnosticsPanel : public QWidget {
  Q_OBJECT

public:
  DiagnosticsPanel(const Document& document,
                   DiagnosticsModel& model,
                   SelectionModel& selection,
                   QWidget* parent = nullptr);

  [[nodiscard]] QTableView* view() { return view_; }

private:
  void on_double_click(const QModelIndex& index);

  const Document& document_;
  DiagnosticsModel& model_;
  SelectionModel& selection_;
  QTableView* view_;
};

} // namespace roadmaker::editor
