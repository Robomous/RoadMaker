// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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
