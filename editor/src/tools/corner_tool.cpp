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

#include "tools/corner_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// Floors for the authored values. The kernel clamps at mesh time, but a drag
/// must never ASK for a degenerate corner: set_corner_extents rejects a
/// non-positive leg outright, and a sub-half-metre radius is not a corner
/// anybody means to author (clearing an override is the properties pane's job,
/// not a gesture's).
constexpr double kMinRadius = 0.5;
constexpr double kMinExtent = 0.05;

void append_segment(std::vector<double>& lines,
                    const std::array<double, 2>& a,
                    const std::array<double, 2>& b) {
  lines.insert(lines.end(), {a[0], a[1], 0.0, b[0], b[1], 0.0});
}

double distance(const std::array<double, 2>& p, double x, double y) {
  return std::hypot(p[0] - x, p[1] - y);
}

/// How far the cursor is from the corner's drawn curve (its apex when the
/// solve produced no samples) — the hover hit-test metric.
double distance_to_corner(const JunctionCornerInfo& info, double x, double y) {
  if (info.curve.empty()) {
    return distance(info.apex(), x, y);
  }
  double best = std::numeric_limits<double>::infinity();
  for (const std::array<double, 2>& point : info.curve) {
    best = std::min(best, distance(point, x, y));
  }
  return best;
}

/// Clamps an authored leg into the range the corner leaves room for. `hi` is
/// widened to the floor so a corner tighter than kMinExtent still yields a
/// legal (if clamped) request rather than an inverted std::clamp range.
double clamp_leg(double value, double lo, double hi) {
  return std::clamp(value, lo, std::max(hi, lo));
}

} // namespace

CornerTool::CornerTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network: every JunctionId the tool is holding
  // becomes stale (and can alias a fresh junction), so drop the lot.
  connect(&document_, &Document::loaded, this, [this] { reset_all(); });
}

void CornerTool::activate() {}

void CornerTool::deactivate() {
  if (press_.has_value() && document_.preview_active()) {
    document_.cancel_preview();
  }
  reset_all();
}

void CornerTool::reset_all() {
  const bool had_active = active_.has_value();
  press_.reset();
  active_.reset();
  hovered_.reset();
  hovered_handle_ = CornerHandle::None;
  if (had_active) {
    emit corner_selection_changed();
  }
  emit preview_changed();
}

std::optional<JunctionCornerInfo> CornerTool::solve(const ActiveCorner& corner) const {
  const std::vector<JunctionCornerInfo> corners =
      junction_corners(document_.network(), corner.junction);
  for (const JunctionCornerInfo& info : corners) {
    if (info.arm_a == corner.arm_a && info.arm_b == corner.arm_b) {
      return info;
    }
  }
  return std::nullopt;
}

std::optional<JunctionCornerInfo> CornerTool::active_corner_info() const {
  return active_.has_value() ? solve(*active_) : std::nullopt;
}

std::optional<ActiveCorner> CornerTool::resolve_corner(const ToolEvent& event) const {
  const RoadNetwork& network = document_.network();

  // A junction floor under the cursor names the junction outright: take its
  // nearest corner whatever the distance, so a click anywhere on the floor
  // still lands on something editable.
  if (event.pick.has_value() && event.pick->junction.is_valid()) {
    std::optional<ActiveCorner> best;
    double best_distance = std::numeric_limits<double>::infinity();
    const std::vector<JunctionCornerInfo> corners = junction_corners(network, event.pick->junction);
    for (const JunctionCornerInfo& info : corners) {
      const double d = distance_to_corner(info, event.world_x, event.world_y);
      if (d < best_distance) {
        best_distance = d;
        best = ActiveCorner{
            .junction = event.pick->junction, .arm_a = info.arm_a, .arm_b = info.arm_b};
      }
    }
    if (best.has_value()) {
      return best;
    }
  }

  // Off the floor (the fillet notch itself is OUTSIDE the pavement): the
  // nearest solved corner within the pick radius, across every junction.
  std::optional<ActiveCorner> best;
  double best_distance = pick_radius_;
  network.for_each_junction([&](JunctionId id, const Junction&) {
    const std::vector<JunctionCornerInfo> corners = junction_corners(network, id);
    for (const JunctionCornerInfo& info : corners) {
      const double d = distance_to_corner(info, event.world_x, event.world_y);
      if (d <= best_distance) {
        best_distance = d;
        best = ActiveCorner{.junction = id, .arm_a = info.arm_a, .arm_b = info.arm_b};
      }
    }
  });
  return best;
}

CornerHandle
CornerTool::pick_handle(const JunctionCornerInfo& info, double world_x, double world_y) const {
  CornerHandle best = CornerHandle::None;
  double best_distance = pick_radius_;
  const auto consider = [&](CornerHandle handle, const std::array<double, 2>& at) {
    const double d = distance(at, world_x, world_y);
    if (d <= best_distance) {
      best_distance = d;
      best = handle;
    }
  };
  consider(CornerHandle::Apex, info.apex());
  consider(CornerHandle::ExtentA, info.tangent_a);
  consider(CornerHandle::ExtentB, info.tangent_b);
  return best;
}

double CornerTool::radius_for(const JunctionCornerInfo& info, double world_x, double world_y) {
  const double half_sin = std::sin(info.phi * 0.5);
  if (half_sin <= 1e-9 || half_sin >= 1.0 - 1e-9) {
    return info.radius; // degenerate/straight pair — the drag has no leverage
  }
  // The apex sits on the inward bisector at d = R (1 - sin(phi/2)) / sin(phi/2)
  // from the edge-line intersection; invert that for the radius the cursor asks
  // for (projected onto the bisector, so off-axis travel is ignored).
  const double along = ((world_x - info.corner[0]) * info.bisector[0]) +
                       ((world_y - info.corner[1]) * info.bisector[1]);
  const double radius = along * half_sin / (1.0 - half_sin);
  return clamp_leg(radius, kMinRadius, info.max_radius);
}

std::array<double, 2> CornerTool::extents_for(const JunctionCornerInfo& info,
                                              CornerHandle handle,
                                              double world_x,
                                              double world_y) {
  const double dx = world_x - info.corner[0];
  const double dy = world_y - info.corner[1];
  // Tangency points are `corner - extent * dir`, so the extent is the cursor's
  // projection measured BACK along the inward edge direction.
  const double along_a = -((dx * info.dir_a[0]) + (dy * info.dir_a[1]));
  const double along_b = -((dx * info.dir_b[0]) + (dy * info.dir_b[1]));

  // The first extent drag must author BOTH sides: the corner is symmetric until
  // now, and storing only the dragged leg would let the other snap to whatever
  // a half-written override implies.
  const double a = handle == CornerHandle::ExtentA ? along_a : info.extent_a;
  const double b = handle == CornerHandle::ExtentB ? along_b : info.extent_b;
  return {clamp_leg(a, kMinExtent, info.max_extent_a), clamp_leg(b, kMinExtent, info.max_extent_b)};
}

void CornerTool::set_active(std::optional<ActiveCorner> corner) {
  const bool changed = corner != active_;
  active_ = std::move(corner);
  if (active_.has_value()) {
    // The rest of the UI follows the junction (there is no corner-level
    // selection entry); Replace so a click reads like every other click.
    selection_.select(SelectionEntry{.junction = active_->junction}, SelectMode::Replace);
  }
  if (changed) {
    emit corner_selection_changed();
  }
  emit preview_changed();
}

bool CornerTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || press_.has_value()) {
    return false;
  }

  // The active corner's handles win over re-resolving a corner: they sit ON
  // the corner they belong to.
  if (active_.has_value()) {
    if (const std::optional<JunctionCornerInfo> info = solve(*active_)) {
      const CornerHandle handle = pick_handle(*info, event.world_x, event.world_y);
      if (handle != CornerHandle::None) {
        // Armed, not previewing: a press that never moves must leave the undo
        // stack alone, so the session only starts on the first move.
        press_ = PressState{.handle = handle, .info = *info};
        emit preview_changed();
        return true;
      }
    }
  }

  if (std::optional<ActiveCorner> corner = resolve_corner(event)) {
    set_active(std::move(corner));
    emit status_message(tr("Drag the apex to set the corner radius, or a tangency point to set "
                           "that side's extent"));
    return true;
  }

  set_active(std::nullopt);
  return false; // nothing here — let the viewport keep the click
}

bool CornerTool::mouse_move(const ToolEvent& event) {
  if (press_.has_value()) {
    update_drag(event.world_x, event.world_y);
    return true;
  }

  std::optional<ActiveCorner> hovered = resolve_corner(event);
  CornerHandle handle = CornerHandle::None;
  if (active_.has_value()) {
    if (const std::optional<JunctionCornerInfo> info = solve(*active_)) {
      handle = pick_handle(*info, event.world_x, event.world_y);
    }
  }
  if (hovered != hovered_ || handle != hovered_handle_) {
    hovered_ = std::move(hovered);
    hovered_handle_ = handle;
    emit preview_changed();
  }
  return false; // hovering never consumes: camera nav and the readout stay live
}

void CornerTool::update_drag(double world_x, double world_y) {
  if (!active_.has_value()) {
    return;
  }
  const ActiveCorner corner = *active_;
  Document::PreviewFactory factory;
  if (press_->handle == CornerHandle::Apex) {
    const double radius = radius_for(press_->info, world_x, world_y);
    factory = [corner, radius](const RoadNetwork& network) {
      return edit::set_corner_radius(network, corner.junction, corner.arm_a, corner.arm_b, radius);
    };
  } else {
    const std::array<double, 2> extents =
        extents_for(press_->info, press_->handle, world_x, world_y);
    factory = [corner, extents](const RoadNetwork& network) {
      return edit::set_corner_extents(
          network, corner.junction, corner.arm_a, corner.arm_b, extents[0], extents[1]);
    };
  }

  if (!document_.preview_active()) {
    // First move of the gesture opens the session.
    if (!document_.begin_preview(factory(document_.network())).has_value()) {
      return; // still armed: a later move can recover
    }
  } else if (!document_.update_preview(factory).has_value()) {
    return; // session stays at its last good frame
  }
  emit preview_changed();
}

bool CornerTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!press_.has_value()) {
    return false;
  }
  // A press that never moved never opened a session, so this commits nothing.
  const bool edited = document_.preview_active();
  const bool was_radius = press_->handle == CornerHandle::Apex;
  document_.commit_preview();
  press_.reset();
  if (edited) {
    emit status_message(was_radius ? tr("Corner radius set — Ctrl+Z to undo")
                                   : tr("Corner extent set — Ctrl+Z to undo"));
  }
  emit preview_changed();
  return true;
}

bool CornerTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key != Qt::Key_Escape) {
    return false;
  }
  if (press_.has_value()) {
    if (document_.preview_active()) {
      document_.cancel_preview();
    }
    press_.reset();
    emit status_message(tr("Corner edit cancelled"));
    emit preview_changed();
    return true;
  }
  if (active_.has_value()) {
    set_active(std::nullopt);
    return true;
  }
  return false;
}

PreviewGeometry CornerTool::preview() const {
  PreviewGeometry geometry;

  const auto outline = [&geometry](const JunctionCornerInfo& info) {
    for (std::size_t i = 0; i + 1 < info.curve.size(); ++i) {
      append_segment(geometry.line_positions, info.curve[i], info.curve[i + 1]);
    }
    // The extent guides: each arm's face corner out to its tangency point,
    // dashed so they read as measurement rather than pavement.
    append_segment(geometry.dashed_line_positions, info.face_a, info.tangent_a);
    append_segment(geometry.dashed_line_positions, info.face_b, info.tangent_b);
  };

  std::optional<JunctionCornerInfo> active_info;
  if (active_.has_value()) {
    active_info = solve(*active_);
    if (active_info.has_value()) {
      outline(*active_info);
    }
  }
  if (hovered_.has_value() && hovered_ != active_) {
    if (const std::optional<JunctionCornerInfo> info = solve(*hovered_)) {
      outline(*info);
    }
  }

  if (active_info.has_value()) {
    const auto state = [this](CornerHandle handle) {
      if (press_.has_value() && press_->handle == handle) {
        return HandleState::Grabbed;
      }
      return hovered_handle_ == handle ? HandleState::Hovered : HandleState::Idle;
    };
    const std::array<double, 2> apex = active_info->apex();
    geometry.add_handle(apex[0], apex[1], 0.0, HandleKind::Node, state(CornerHandle::Apex));
    geometry.add_handle(active_info->tangent_a[0],
                        active_info->tangent_a[1],
                        0.0,
                        HandleKind::Node,
                        state(CornerHandle::ExtentA));
    geometry.add_handle(active_info->tangent_b[0],
                        active_info->tangent_b[1],
                        0.0,
                        HandleKind::Node,
                        state(CornerHandle::ExtentB));
  }

  return geometry;
}

QString CornerTool::instruction() const {
  return tr("Click a junction corner, then drag its apex to set the radius or a tangency point to "
            "set that side's extent · Esc cancels");
}

} // namespace roadmaker::editor
