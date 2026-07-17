#include "tools/lane_profile_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QKeyEvent>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// A lane is removable exactly when the kernel's edit::remove_lane accepts it:
/// non-center and the outermost lane of its side (M2 restriction). Mirrored
/// from context_menu.cpp's lane_removable so the gesture and the menu agree;
/// the kernel stays the final arbiter (a refused command appends a diagnostic).
bool lane_removable(const RoadNetwork& network, LaneId lane_id) {
  const Lane* lane = network.lane(lane_id);
  if (lane == nullptr || lane->odr_id == 0) {
    return false;
  }
  const LaneSection* section = network.lane_section(lane->section);
  if (section == nullptr) {
    return false;
  }
  for (const LaneId other_id : section->lanes) {
    const int other = network.lane(other_id)->odr_id;
    if ((lane->odr_id > 0 && other > lane->odr_id) || (lane->odr_id < 0 && other < lane->odr_id)) {
      return false; // a lane sits further out — not the outermost
    }
  }
  return true;
}

} // namespace

LaneProfileTool::LaneProfileTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void LaneProfileTool::activate() {}

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

namespace {

/// Cycles the lane travel direction Standard -> Reversed -> Both -> Standard.
LaneDirection next_direction(LaneDirection current) {
  switch (current) {
  case LaneDirection::Standard:
    return LaneDirection::Reversed;
  case LaneDirection::Reversed:
    return LaneDirection::Both;
  case LaneDirection::Both:
    return LaneDirection::Standard;
  }
  return LaneDirection::Standard;
}

QString direction_label(LaneDirection direction) {
  switch (direction) {
  case LaneDirection::Standard:
    return LaneProfileTool::tr("standard");
  case LaneDirection::Reversed:
    return LaneProfileTool::tr("reversed");
  case LaneDirection::Both:
    return LaneProfileTool::tr("both");
  }
  return LaneProfileTool::tr("standard");
}

} // namespace

bool LaneProfileTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  if (key == Qt::Key_D && (modifiers & Qt::ShiftModifier)) {
    return cycle_direction();
  }
  if (key != Qt::Key_Delete && key != Qt::Key_Backspace) {
    return false;
  }
  const LaneId lane_id = selection_.primary().lane;
  if (!lane_id.is_valid()) {
    emit status_message(tr("Select a lane to delete."));
    return true;
  }
  if (!lane_removable(document_.network(), lane_id)) {
    // The kernel only removes the outermost, non-center lane of a side. Say so
    // rather than let Delete no-op silently on a centre or interior lane.
    emit status_message(
        tr("Only the outermost lane of a side can be deleted (not the centre lane)."));
    return true;
  }
  const int odr = document_.network().lane(lane_id)->odr_id;
  if (document_.push_command(edit::remove_lane(document_.network(), lane_id))) {
    emit status_message(tr("Removed lane %1.").arg(odr));
  }
  return true;
}

bool LaneProfileTool::cycle_direction() {
  const LaneId lane_id = selection_.primary().lane;
  if (!lane_id.is_valid()) {
    emit status_message(tr("Select a lane to change its travel direction."));
    return true;
  }
  const Lane* lane = document_.network().lane(lane_id);
  if (lane == nullptr || lane->odr_id == 0) {
    // The kernel refuses the center lane (it has no travel direction); say so
    // rather than push a command that would be rejected.
    emit status_message(tr("The centre lane has no travel direction."));
    return true;
  }
  const LaneDirection next = next_direction(lane->direction);
  const int odr = lane->odr_id;
  if (document_.push_command(edit::set_lane_direction(document_.network(), lane_id, next))) {
    emit status_message(tr("Lane %1 direction: %2.").arg(odr).arg(direction_label(next)));
  }
  return true;
}

QString LaneProfileTool::instruction() const {
  return tr("Click a lane to select it. Delete removes it. Shift+D cycles its travel "
            "direction. Edit type in Properties; edit width along s in the 2D Editor (Width).");
}

} // namespace roadmaker::editor
