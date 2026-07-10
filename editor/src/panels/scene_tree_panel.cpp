#include "panels/scene_tree_panel.hpp"

#include <QItemSelectionModel>
#include <QVBoxLayout>

namespace roadmaker::editor {

SceneTreePanel::SceneTreePanel(SceneTreeModel& model, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), model_(model), selection_(selection), view_(new QTreeView(this)) {
  view_->setModel(&model_);
  view_->setHeaderHidden(false);
  view_->setSelectionMode(QAbstractItemView::SingleSelection);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->expandToDepth(0);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(view_);

  connect(view_->selectionModel(),
          &QItemSelectionModel::currentChanged,
          this,
          [this](const QModelIndex& current, const QModelIndex&) { on_view_selection(current); });
  connect(
      &selection_, &SelectionModel::selection_changed, this, &SceneTreePanel::on_model_selection);
  connect(&model_, &QAbstractItemModel::modelReset, this, [this] { view_->expandToDepth(0); });
}

void SceneTreePanel::on_view_selection(const QModelIndex& index) {
  if (mirroring_) {
    return;
  }
  const SceneTreeModel::Target target = model_.target_for(index);
  mirroring_ = true;
  if (target.lane.is_valid()) {
    selection_.select_lane(target.road, target.lane);
  } else if (target.road.is_valid()) {
    selection_.select_road(target.road);
  } else {
    selection_.clear(); // group header
  }
  mirroring_ = false;
}

void SceneTreePanel::on_model_selection(RoadId road, LaneId lane) {
  if (mirroring_) {
    return;
  }
  const QModelIndex index =
      lane.is_valid() ? model_.index_for_lane(lane) : model_.index_for_road(road);
  mirroring_ = true;
  if (index.isValid()) {
    view_->setCurrentIndex(index);
    view_->scrollTo(index);
  } else {
    view_->clearSelection();
    view_->setCurrentIndex({});
  }
  mirroring_ = false;
}

} // namespace roadmaker::editor
