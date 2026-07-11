#include "tools/lane_profile_tool.hpp"

#include "document/selection_model.hpp"

namespace roadmaker::editor {

LaneProfileTool::LaneProfileTool(SelectionModel& selection, QObject* parent)
    : Tool(parent), selection_(selection) {}

void LaneProfileTool::activate() {
  emit status_message(
      tr("Lane Profile — click a lane to edit its cross-section in the Properties panel"));
}

bool LaneProfileTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  if (event.pick.has_value()) {
    selection_.select({.road = event.pick->road, .lane = event.pick->lane});
  } else {
    selection_.clear();
  }
  return true; // LMB belongs to the tool even on a miss (M2 button map)
}

} // namespace roadmaker::editor
