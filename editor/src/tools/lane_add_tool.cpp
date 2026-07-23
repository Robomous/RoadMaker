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

#include "tools/lane_add_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <QKeyEvent>
#include <algorithm>
#include <cmath>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {
constexpr double kMinSpan = 1.0; // a drag shorter than this is a click, not a span [m]

/// The side (+1 left / -1 right) the pocket belongs to: the sign of the picked
/// lane's OpenDRIVE id, falling back to the cursor's t sign for a centre pick.
int side_from_pick(const RoadNetwork& network, const ToolEvent& event) {
  if (event.pick.has_value()) {
    const Lane* lane = network.lane(event.pick->lane);
    if (lane != nullptr && lane->odr_id != 0) {
      return lane->odr_id > 0 ? 1 : -1;
    }
  }
  const double t = event.station.has_value() ? event.station->t : 0.0;
  return t >= 0.0 ? 1 : -1;
}
} // namespace

LaneAddTool::LaneAddTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void LaneAddTool::reset() {
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  pressed_ = false;
  span_valid_ = false;
  road0_ = RoadId{};
  emit preview_changed();
}

void LaneAddTool::deactivate() {
  // ACCEPTANCE-CRITICAL: an in-flight preview must be cancelled here, or the
  // leaked session refuses the next tool's push_command.
  reset();
}

bool LaneAddTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // A pocket needs a road body and a station; a miss clears any live drag but
  // still belongs to the tool (M2 button map).
  if (!event.pick.has_value() || !event.pick->road.is_valid() || !event.station.has_value()) {
    reset();
    return true;
  }
  road0_ = event.pick->road;
  s0_ = event.station->s;
  side_ = side_from_pick(document_.network(), event);
  pressed_ = true;
  span_valid_ = false;
  lo_ = hi_ = s0_;
  return true;
}

bool LaneAddTool::mouse_move(const ToolEvent& event) {
  if (!pressed_) {
    return false;
  }
  // The drag must stay on the road the press started on — a span crosses one
  // road only. A stray move off the road just holds the last good span.
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
  const auto factory = [road, side, lo, hi](const RoadNetwork& base) {
    return edit::add_lane_span(base, road, side, lo, hi, LaneType::Driving);
  };
  // A kernel refusal (e.g. a span that still collapses once clamped) just holds
  // the last good frame — the drag is the tool's either way.
  if (document_.preview_active()) {
    static_cast<void>(document_.update_preview(factory));
  } else {
    static_cast<void>(document_.begin_preview(
        edit::add_lane_span(document_.network(), road, side, lo, hi, LaneType::Driving)));
  }
  emit preview_changed();
  return true;
}

bool LaneAddTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!pressed_) {
    return false;
  }
  if (document_.preview_active()) {
    document_.commit_preview();
    emit status_message(tr("Added a lane pocket — Ctrl+Z to undo"));
  } else if (span_valid_) {
    emit status_message(tr("Could not add a lane there."));
  } else {
    emit status_message(tr("Drag along a road to size the lane pocket."));
  }
  // Stay active for the next pocket; only the drag state resets.
  pressed_ = false;
  span_valid_ = false;
  road0_ = RoadId{};
  emit preview_changed();
  return true;
}

bool LaneAddTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    reset();
    return true;
  }
  return false;
}

PreviewGeometry LaneAddTool::preview() const {
  PreviewGeometry geometry;
  if (!span_valid_ || !road0_.is_valid()) {
    return geometry;
  }
  const Road* road = document_.network().road(road0_);
  if (road == nullptr) {
    return geometry;
  }
  // A band along the reference line between the seams, drawn as consecutive
  // accent segments — a lightweight highlight of where the pocket will land.
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

QString LaneAddTool::instruction() const {
  return tr("Drag along a road to add a lane pocket over that span · Esc cancels");
}

} // namespace roadmaker::editor
