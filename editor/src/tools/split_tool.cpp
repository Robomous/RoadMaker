#include "tools/split_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <cmath>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {
constexpr double kMarkerRadius = 1.2; // cut-cross half-size [m]
} // namespace

SplitTool::SplitTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void SplitTool::activate() {
  emit status_message(tr("Click a road to split it at the cut marker — Esc to cancel"));
}

void SplitTool::deactivate() {
  if (hover_.has_value()) {
    hover_.reset();
    emit preview_changed();
  }
}

std::optional<SplitTool::CutHover> SplitTool::hover_at(const ToolEvent& event) const {
  if (!event.pick.has_value()) {
    return std::nullopt;
  }
  const Road* road = document_.network().road(event.pick->road);
  if (road == nullptr) {
    return std::nullopt;
  }
  const StationCoord coord = find_station(road->plan_view, event.world_x, event.world_y);
  const PathPoint at = road->plan_view.evaluate(coord.s);
  return CutHover{.road = event.pick->road, .s = coord.s, .position = {.x = at.x, .y = at.y}};
}

bool SplitTool::mouse_move(const ToolEvent& event) {
  hover_ = hover_at(event);
  emit preview_changed();
  return hover_.has_value();
}

bool SplitTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  const std::optional<CutHover> cut = hover_at(event);
  if (!cut.has_value()) {
    return false;
  }
  const RoadId original = cut->road;
  const Expected<void> split =
      document_.push_command(edit::split_road(document_.network(), original, cut->s));
  if (!split.has_value()) {
    emit status_message(tr("Cannot split: %1").arg(QString::fromStdString(split.error().message)));
    return true;
  }

  // The new tail is the dirty road that is not the original.
  RoadId tail;
  for (const RoadId dirty : document_.last_dirty().roads) {
    if (dirty != original) {
      tail = dirty;
      break;
    }
  }
  selection_.clear();
  selection_.select({.road = original, .lane = LaneId{}}, SelectMode::Replace);
  if (tail.is_valid()) {
    selection_.select({.road = tail, .lane = LaneId{}}, SelectMode::Add);
  }
  const Road* head_road = document_.network().road(original);
  const Road* tail_road = tail.is_valid() ? document_.network().road(tail) : nullptr;
  emit status_message(head_road != nullptr && tail_road != nullptr
                          ? tr("Split into road %1 and road %2 — Ctrl+Z to undo")
                                .arg(QString::fromStdString(head_road->odr_id))
                                .arg(QString::fromStdString(tail_road->odr_id))
                          : tr("Road split — Ctrl+Z to undo"));

  hover_.reset();
  emit request_tool(ToolId::Select); // one-shot
  return true;
}

bool SplitTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    emit request_tool(ToolId::Select);
    return true;
  }
  return false;
}

PreviewGeometry SplitTool::preview() const {
  PreviewGeometry geometry;
  if (!hover_.has_value()) {
    return geometry;
  }
  const double x = hover_->position.x;
  const double y = hover_->position.y;
  // An X marker at the cut point.
  geometry.line_positions.insert(geometry.line_positions.end(),
                                 {x - kMarkerRadius,
                                  y - kMarkerRadius,
                                  0.0,
                                  x + kMarkerRadius,
                                  y + kMarkerRadius,
                                  0.0,
                                  x - kMarkerRadius,
                                  y + kMarkerRadius,
                                  0.0,
                                  x + kMarkerRadius,
                                  y - kMarkerRadius,
                                  0.0});
  return geometry;
}

} // namespace roadmaker::editor
