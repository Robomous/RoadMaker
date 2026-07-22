// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "panels/diagnostics_panel.hpp"

#include <QHeaderView>
#include <QVBoxLayout>

namespace roadmaker::editor {

DiagnosticsPanel::DiagnosticsPanel(const Document& document,
                                   DiagnosticsModel& model,
                                   SelectionModel& selection,
                                   QWidget* parent)
    : QWidget(parent), document_(document), model_(model), selection_(selection),
      view_(new QTableView(this)) {
  view_->setModel(&model_);
  view_->setSelectionBehavior(QAbstractItemView::SelectRows);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->verticalHeader()->hide();
  view_->horizontalHeader()->setStretchLastSection(true);
  view_->setAlternatingRowColors(true);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(view_);

  connect(view_, &QTableView::doubleClicked, this, &DiagnosticsPanel::on_double_click);
}

void DiagnosticsPanel::on_double_click(const QModelIndex& index) {
  const Diagnostic* diagnostic = model_.diagnostic_at(index.row());
  if (diagnostic == nullptr || document_.network().road(diagnostic->road) == nullptr) {
    return; // no entity attached — navigation is best effort by design
  }
  const bool lane_live = document_.network().lane(diagnostic->lane) != nullptr;
  selection_.select({.road = diagnostic->road, .lane = lane_live ? diagnostic->lane : LaneId{}});
}

} // namespace roadmaker::editor
