#pragma once

// Lane Profile tool (issue #14, docs/design/m2/02_editing_tools.md §4). The
// editing itself is panel-based (PropertiesPanel's lane-profile section
// follows the primary selection); the tool exists for toolbar consistency
// and lane-granular viewport highlighting: a click selects the picked LANE,
// empty space clears.

#include "tools/tool.hpp"

namespace roadmaker::editor {

class SelectionModel;

class LaneProfileTool : public Tool {
  Q_OBJECT

public:
  explicit LaneProfileTool(SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;

  [[nodiscard]] QString instruction() const override;

private:
  SelectionModel& selection_;
};

} // namespace roadmaker::editor
