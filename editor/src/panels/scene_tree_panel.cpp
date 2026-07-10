#include "panels/scene_tree_panel.hpp"

#include <QItemSelectionModel>
#include <QVBoxLayout>

namespace roadmaker::editor {

namespace {

SelectionEntry entry_for(const SceneTreeModel::Target& target) {
  return {.road = target.road, .lane = target.lane};
}

} // namespace

SceneTreePanel::SceneTreePanel(SceneTreeModel& model, SelectionModel& selection, QWidget* parent)
    : QWidget(parent), model_(model), selection_(selection), view_(new QTreeView(this)) {
  view_->setModel(&model_);
  view_->setHeaderHidden(false);
  view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  view_->setEditTriggers(QAbstractItemView::NoEditTriggers);
  view_->expandToDepth(0);

  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(view_);

  connect(view_->selectionModel(),
          &QItemSelectionModel::selectionChanged,
          this,
          [this](const QItemSelection&, const QItemSelection&) { on_view_selection(); });
  connect(
      &selection_, &SelectionModel::selection_changed, this, &SceneTreePanel::on_model_selection);
  connect(&model_, &QAbstractItemModel::modelReset, this, [this] { view_->expandToDepth(0); });
}

void SceneTreePanel::on_view_selection() {
  if (mirroring_) {
    return;
  }

  // Rows resolving to an entity, in view order; group headers contribute
  // nothing. The current row goes last so it becomes the primary.
  std::vector<SelectionEntry> entries;
  SelectionEntry current_entry;
  const QModelIndex current = view_->currentIndex();
  for (const QModelIndex& index : view_->selectionModel()->selectedRows()) {
    const SelectionEntry entry = entry_for(model_.target_for(index));
    if (!entry.road.is_valid()) {
      continue;
    }
    if (index == current) {
      current_entry = entry;
      continue;
    }
    entries.push_back(entry);
  }
  if (current_entry.road.is_valid()) {
    entries.push_back(current_entry);
  }

  mirroring_ = true;
  if (entries.empty()) {
    selection_.clear(); // group headers only
  } else {
    selection_.select_many(entries, SelectMode::Replace);
  }
  mirroring_ = false;
}

void SceneTreePanel::on_model_selection() {
  if (mirroring_) {
    return;
  }

  QItemSelection view_selection;
  QModelIndex primary_index;
  for (const SelectionEntry& entry : selection_.entries()) {
    const QModelIndex index = entry.lane.is_valid() ? model_.index_for_lane(entry.lane)
                                                    : model_.index_for_road(entry.road);
    if (!index.isValid()) {
      continue;
    }
    view_selection.select(index, index);
    if (entry == selection_.primary()) {
      primary_index = index;
    }
  }

  mirroring_ = true;
  view_->selectionModel()->select(view_selection,
                                  QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  view_->selectionModel()->setCurrentIndex(primary_index, QItemSelectionModel::NoUpdate);
  if (primary_index.isValid()) {
    view_->scrollTo(primary_index);
  }
  mirroring_ = false;
}

} // namespace roadmaker::editor
