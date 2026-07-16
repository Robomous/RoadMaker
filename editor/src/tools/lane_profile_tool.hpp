#pragma once

// Lane tool (issue #14 / p2-s4). The editing itself is panel-based: type edits
// in the Properties panel, width-along-s in the 2D Editor's Lane Width tab; the
// tool exists for toolbar consistency, lane-granular viewport highlighting, and
// the one direct gesture it owns — Delete removes the selected lane. A click
// selects the picked LANE, empty space clears.

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class LaneProfileTool : public Tool {
  Q_OBJECT

public:
  LaneProfileTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;

  /// Delete/Backspace removes the primary lane through edit::remove_lane (ONE
  /// undo step). A null/center/non-outermost lane can't be removed — the
  /// gesture then emits a status message rather than doing nothing silently.
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] QString instruction() const override;

private:
  Document& document_;
  SelectionModel& selection_;
};

} // namespace roadmaker::editor
