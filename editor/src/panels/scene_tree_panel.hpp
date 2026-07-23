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
