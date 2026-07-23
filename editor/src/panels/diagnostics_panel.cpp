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
