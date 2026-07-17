#include "tools/lane_carve_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QKeyEvent>
#include <algorithm>
#include <optional>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {
constexpr double kMinSpan = 1.0; // a drag shorter than this is a click, not a taper [m]
} // namespace

LaneCarveTool::LaneCarveTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void LaneCarveTool::reset() {
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  pressed_ = false;
  span_valid_ = false;
  road0_ = RoadId{};
  emit preview_changed();
}

void LaneCarveTool::deactivate() {
  // ACCEPTANCE-CRITICAL: an in-flight preview must be cancelled here, or the
  // leaked session refuses the next tool's push_command.
  reset();
}

bool LaneCarveTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // A carve needs a road body and a station; a miss clears any live drag but
  // still belongs to the tool (M2 button map).
  if (!event.pick.has_value() || !event.pick->road.is_valid() || !event.station.has_value()) {
    reset();
    return true;
  }
  // The turn lane is inserted at the lane boundary nearest the press.
  const auto boundary = nearest_lane_boundary(
      document_.network(), event.pick->road, event.station->s, event.station->t);
  if (!boundary.has_value()) {
    reset();
    return true;
  }
  road0_ = event.pick->road;
  s0_ = event.station->s;
  side_ = boundary->side;
  at_odr_id_ = boundary->at_odr_id;
  pressed_ = true;
  span_valid_ = false;
  lo_ = hi_ = s0_;
  return true;
}

bool LaneCarveTool::mouse_move(const ToolEvent& event) {
  if (!pressed_) {
    return false;
  }
  // The drag must stay on the road the press started on. A stray move off the
  // road just holds the last good taper span.
  if (!event.pick.has_value() || event.pick->road != road0_ || !event.station.has_value()) {
    return true;
  }
  const double s1 = event.station->s;
  const double lo = std::min(s0_, s1);
  const double hi = std::max(s0_, s1);
  if (hi - lo <= kMinSpan) {
    return true; // still a click-sized nudge
  }
  lo_ = lo;
  hi_ = hi;
  span_valid_ = true;

  const RoadId road = road0_;
  const int side = side_;
  const int at_odr_id = at_odr_id_;
  const auto factory = [road, side, lo, hi, at_odr_id](const RoadNetwork& base) {
    return edit::carve_lane(base, road, side, lo, hi, at_odr_id, LaneType::Driving);
  };
  // A kernel refusal (e.g. carving away from the final section) just holds the
  // last good frame — the drag is the tool's either way.
  if (document_.preview_active()) {
    static_cast<void>(document_.update_preview(factory));
  } else {
    static_cast<void>(document_.begin_preview(
        edit::carve_lane(document_.network(), road, side, lo, hi, at_odr_id, LaneType::Driving)));
  }
  emit preview_changed();
  return true;
}

bool LaneCarveTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!pressed_) {
    return false;
  }
  if (document_.preview_active()) {
    document_.commit_preview();
    emit status_message(tr("Carved a turn lane — Ctrl+Z to undo"));
  } else if (span_valid_) {
    emit status_message(tr("Could not carve a turn lane there — carve nearer the junction end."));
  } else {
    emit status_message(tr("Drag along a lane toward the junction to size the turn lane."));
  }
  // Stay active for the next carve; only the drag state resets.
  pressed_ = false;
  span_valid_ = false;
  road0_ = RoadId{};
  emit preview_changed();
  return true;
}

bool LaneCarveTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    reset();
    return true;
  }
  return false;
}

PreviewGeometry LaneCarveTool::preview() const {
  PreviewGeometry geometry;
  if (!span_valid_ || !road0_.is_valid()) {
    return geometry;
  }
  const Road* road = document_.network().road(road0_);
  if (road == nullptr) {
    return geometry;
  }
  // A band along the reference line over the taper span, drawn as consecutive
  // accent segments — a lightweight highlight of where the turn lane will land.
  constexpr int kSamples = 24;
  std::optional<PathPoint> prev;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = lo_ + (hi_ - lo_) * i / kSamples;
    const PathPoint at = road->plan_view.evaluate(s);
    if (prev.has_value()) {
      geometry.line_positions.insert(geometry.line_positions.end(),
                                     {prev->x, prev->y, 0.0, at.x, at.y, 0.0});
    }
    prev = at;
  }
  return geometry;
}

QString LaneCarveTool::instruction() const {
  return tr("Drag along a lane toward the junction to carve a tapering turn lane · Esc cancels");
}

} // namespace roadmaker::editor
