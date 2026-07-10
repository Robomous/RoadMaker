#include "panels/diagnostics_panel.hpp"

#include <QHeaderView>
#include <QVBoxLayout>

#include "document/diagnostic_locator.hpp"

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
  if (diagnostic == nullptr) {
    return;
  }
  const auto target = resolve_diagnostic_location(document_.network(), diagnostic->location);
  if (!target) {
    return; // unresolvable location — navigation is best effort by design
  }
  if (target->lane.is_valid()) {
    selection_.select_lane(target->road, target->lane);
  } else {
    selection_.select_road(target->road);
  }
}

} // namespace roadmaker::editor
