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

#include "tools/lane_form_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QKeyEvent>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// The (side, at_odr_id) the formed lane takes: the picked lane's position, so
/// the new lane appears there and the picked lane steps one further out. A
/// centre pick falls back to the innermost lane on the cursor's t side.
std::pair<int, int> form_target(const RoadNetwork& network, const ToolEvent& event) {
  if (event.pick.has_value()) {
    const Lane* lane = network.lane(event.pick->lane);
    if (lane != nullptr && lane->odr_id != 0) {
      const int side = lane->odr_id > 0 ? 1 : -1;
      return {side, lane->odr_id};
    }
  }
  const int side = (event.station.has_value() ? event.station->t : 0.0) >= 0.0 ? 1 : -1;
  return {side, side}; // innermost lane on that side is +/-1
}

} // namespace

LaneFormTool::LaneFormTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void LaneFormTool::reset() {
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  forming_ = false;
  road_ = RoadId{};
  emit preview_changed();
}

void LaneFormTool::deactivate() {
  // ACCEPTANCE-CRITICAL: cancel any uncommitted preview so it cannot leak into
  // the next tool and refuse its push_command.
  reset();
}

bool LaneFormTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  if (!event.pick.has_value() || !event.pick->road.is_valid() || !event.station.has_value()) {
    reset();
    return true;
  }
  const RoadId road = event.pick->road;
  const double s_start = event.station->s;
  const auto [side, at_odr_id] = form_target(document_.network(), event);

  const Expected<void> began = document_.begin_preview(
      edit::form_lane(document_.network(), road, side, s_start, at_odr_id, LaneType::Driving));
  if (!began.has_value()) {
    // The kernel guard (downstream seam, or an out-of-range station) refused —
    // surface it and leave no state.
    reset();
    emit status_message(
        tr("Cannot form a lane here: %1").arg(QString::fromStdString(began.error().message)));
    return true;
  }
  forming_ = true;
  road_ = road;
  s_start_ = s_start;
  emit preview_changed();
  return true;
}

bool LaneFormTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!forming_) {
    return false;
  }
  document_.commit_preview();
  forming_ = false;
  road_ = RoadId{};
  emit status_message(tr("Formed a lane to the road end — Ctrl+Z to undo"));
  emit preview_changed();
  return true;
}

bool LaneFormTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    reset();
    return true;
  }
  return false;
}

PreviewGeometry LaneFormTool::preview() const {
  PreviewGeometry geometry;
  if (!forming_ || !road_.is_valid()) {
    return geometry;
  }
  const Road* road = document_.network().road(road_);
  if (road == nullptr) {
    return geometry;
  }
  // A band along the reference line from the form station to the road end.
  const double length = road->plan_view.length();
  constexpr int kSamples = 24;
  std::optional<PathPoint> prev;
  for (int i = 0; i <= kSamples; ++i) {
    const double s = s_start_ + (length - s_start_) * i / kSamples;
    const PathPoint at = road->plan_view.evaluate(s);
    if (prev.has_value()) {
      geometry.line_positions.insert(geometry.line_positions.end(),
                                     {prev->x, prev->y, 0.0, at.x, at.y, 0.0});
    }
    prev = at;
  }
  return geometry;
}

QString LaneFormTool::instruction() const {
  return tr("Click a road to form a lane from there to its end · Esc cancels");
}

} // namespace roadmaker::editor
