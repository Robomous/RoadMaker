#include "panels/properties_panel.hpp"

#include <QVBoxLayout>

namespace roadmaker::editor {

PropertiesPanel::PropertiesPanel(const Document& document,
                                 const SelectionModel& selection,
                                 QWidget* parent)
    : QWidget(parent), document_(document), selection_(selection), form_(new QFormLayout),
      placeholder_(new QLabel(tr("Select a road or lane."), this)) {
  placeholder_->setWordWrap(true);
  placeholder_->setEnabled(false);

  auto* layout = new QVBoxLayout(this);
  layout->addWidget(placeholder_);
  layout->addLayout(form_);
  layout->addStretch();

  connect(&selection_, &SelectionModel::selection_changed, this, &PropertiesPanel::refresh);
  connect(&document_, &Document::loaded, this, &PropertiesPanel::refresh);
  refresh();
}

void PropertiesPanel::refresh() {
  clear_rows();

  // The primary entry (most recently selected) drives the panel.
  const SelectionEntry primary = selection_.primary();
  const Road* road = document_.network().road(primary.road);
  if (road == nullptr) {
    placeholder_->show();
    return;
  }
  placeholder_->hide();

  if (selection_.entries().size() > 1) {
    add_row(tr("Selection"), tr("%1 items").arg(selection_.entries().size()));
  }
  add_row(tr("Road"),
          road->name.empty() ? QString::fromStdString(road->odr_id)
                             : QString::fromStdString(road->name));
  add_row(tr("OpenDRIVE id"), QString::fromStdString(road->odr_id));
  add_row(tr("Length"), tr("%1 m").arg(road->length, 0, 'f', 3));
  add_row(tr("Geometry records"), QString::number(road->plan_view.records().size()));
  add_row(tr("Lane sections"), QString::number(road->sections.size()));

  const Lane* lane = document_.network().lane(primary.lane);
  if (lane != nullptr) {
    add_row(tr("Lane"), QString::number(lane->odr_id));
    add_row(tr("Width records"), QString::number(lane->widths.size()));
    add_row(tr("Road marks"), QString::number(lane->road_marks.size()));
  }
}

void PropertiesPanel::add_row(const QString& label, const QString& value) {
  auto* value_label = new QLabel(value, this);
  value_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
  form_->addRow(label, value_label);
}

void PropertiesPanel::clear_rows() {
  while (form_->rowCount() > 0) {
    form_->removeRow(0);
  }
}

} // namespace roadmaker::editor
