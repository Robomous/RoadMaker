#pragma once

// Thin QTreeView host over SceneTreeModel. All logic lives in the model and
// SelectionModel; this class only translates view clicks <-> selection with a
// re-entrancy guard against ping-pong.

#include <QTreeView>
#include <QWidget>

#include "document/scene_tree_model.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

class SceneTreePanel : public QWidget {
  Q_OBJECT

public:
  SceneTreePanel(SceneTreeModel& model, SelectionModel& selection, QWidget* parent = nullptr);

  [[nodiscard]] QTreeView* view() { return view_; }

private:
  void on_view_selection();
  void on_model_selection();

  SceneTreeModel& model_;
  SelectionModel& selection_;
  QTreeView* view_;
  bool mirroring_ = false;
};

} // namespace roadmaker::editor
