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

#include "tools/prop_span_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <QRandomGenerator>
#include <QUndoStack>
#include <algorithm>
#include <cmath>
#include <random>
#include <utility>

#include "document/document.hpp"
#include "document/prop_placement.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// A prop span anchors to a road whose reference line passes within this lateral
/// distance [m] of the first click — the same reach the tree drop and prop curve
/// use, so aiming on or just beside the carriageway grabs the road.
constexpr double kAnchorSnapThreshold = kObjectSnapThreshold;

/// Distance keys nudge by this step [m], clamped to [min, max] — a usable spread
/// from tight hedging to sparse avenue trees (matches the Prop Curve clamp).
constexpr double kDistanceStep = 0.5;
constexpr double kDistanceMin = 0.5;
constexpr double kDistanceMax = 50.0;

} // namespace

PropSpanTool::PropSpanTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void PropSpanTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem PropSpanTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

std::size_t PropSpanTool::station_count() const {
  if (!anchor_.has_value()) {
    return 0;
  }
  return s2_.has_value() ? 2 : 1;
}

void PropSpanTool::deactivate() {
  reset_session();
}

bool PropSpanTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  if (!anchor_.has_value()) {
    // The first click anchors the span and pins its lateral offset. Refuse if the
    // asset is incompatible or the click is off any road.
    if (!is_prop_asset(current_item())) {
      emit toast_requested(tr("Select a tree or shrub in the Library first"),
                           ToastSeverity::Warning);
      return true;
    }
    const std::optional<RoadStation> anchor = nearest_road_station(
        document_.network(), event.world_x, event.world_y, kAnchorSnapThreshold);
    if (!anchor.has_value()) {
      emit status_message(tr("Click on or beside a road to start the prop span"));
      return true;
    }
    anchor_ = anchor->road;
    s1_ = anchor->s;
    t_ = anchor->t;
    // A PropSet resolves to ONE model for the whole span (a repeat cannot mix
    // models); a plain Tree resolves to itself. Drawn once, here, so the preview
    // and the commit agree.
    session_seed_ = QRandomGenerator::global()->generate();
    std::mt19937 rng(session_seed_);
    resolved_item_ = resolve_prop_asset(current_item(), rng);
    emit status_message(tr("Click a second station on the same road; Enter commits the span"));
    emit preview_changed();
    return true;
  }
  // A later click sets (or re-sets) the far station — but only when it stays on
  // the anchor road (a span is single-road).
  const Road* road = document_.network().road(*anchor_);
  if (road == nullptr || road->plan_view.empty()) {
    return true;
  }
  const std::optional<StationCoord> station =
      station_within(road->plan_view, event.world_x, event.world_y, kObjectSnapThreshold);
  if (!station.has_value()) {
    emit status_message(tr("Stay on the anchor road to set the span's end"));
    return true;
  }
  s2_ = station->s;
  emit status_message(tr("Span set — Enter or double-click commits, [ / ] set spacing"));
  emit preview_changed();
  return true;
}

bool PropSpanTool::mouse_move(const ToolEvent& event) {
  cursor_s_.reset();
  if (anchor_.has_value()) {
    const Road* road = document_.network().road(*anchor_);
    if (road != nullptr && !road->plan_view.empty()) {
      if (const std::optional<StationCoord> station =
              station_within(road->plan_view, event.world_x, event.world_y, kObjectSnapThreshold)) {
        cursor_s_ = station->s;
      }
    }
  }
  emit preview_changed();
  return false; // hovering never consumes: camera nav and hover readout stay live
}

bool PropSpanTool::mouse_double_click(const ToolEvent& event) {
  static_cast<void>(event); // the pair's first press already set the second station
  commit();
  return true;
}

bool PropSpanTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    commit();
    return true;
  }
  if (key == Qt::Key_BracketLeft) {
    adjust_distance(-kDistanceStep);
    return true;
  }
  if (key == Qt::Key_BracketRight) {
    adjust_distance(kDistanceStep);
    return true;
  }
  if (key == Qt::Key_Backspace) {
    if (s2_.has_value()) {
      s2_.reset(); // step back to just the anchor
      emit status_message(tr("Cleared the span's end"));
      emit preview_changed();
      return true;
    }
    if (anchor_.has_value()) {
      reset_session();
      emit status_message(tr("Cleared the anchor"));
      return true;
    }
    return false;
  }
  if (key == Qt::Key_Escape) {
    if (!anchor_.has_value()) {
      return false;
    }
    reset_session();
    emit status_message(tr("Prop span cancelled"));
    return true;
  }
  return false;
}

void PropSpanTool::adjust_distance(double delta) {
  distance_m_ = std::clamp(distance_m_ + delta, kDistanceMin, kDistanceMax);
  emit status_message(tr("Prop span spacing %1 m").arg(distance_m_, 0, 'f', 1));
  emit preview_changed();
}

void PropSpanTool::commit() {
  if (!anchor_.has_value()) {
    emit status_message(tr("Click on a road to start the prop span"));
    return;
  }
  // The second station may come from the committed click or the current hover.
  const std::optional<double> s2 = s2_.has_value() ? s2_ : cursor_s_;
  if (!s2.has_value()) {
    emit status_message(tr("Click a second station to define the span"));
    return;
  }
  Expected<Object> object = make_prop_span_object(
      resolved_item_, next_object_odr_id(document_.network()), s1_, *s2, t_, distance_m_);
  if (!object.has_value()) {
    emit status_message(
        tr("Cannot place the span: %1").arg(QString::fromStdString(object.error().message)));
    return;
  }
  const std::size_t placed =
      span_preview_points(document_.network(), *anchor_, object->repeats.front()).size();

  // ONE object, ONE command → a single undo entry (no macro needed).
  (void)document_.push_command(edit::add_object(document_.network(), *anchor_, std::move(*object)));

  reset_session();
  emit status_message(
      tr("Prop span: %n prop(s) — Ctrl+Z to undo", nullptr, static_cast<int>(placed)));
}

void PropSpanTool::reset_session() {
  anchor_.reset();
  s1_ = 0.0;
  t_ = 0.0;
  s2_.reset();
  cursor_s_.reset();
  resolved_item_ = LibraryItem{};
  emit preview_changed();
}

PreviewGeometry PropSpanTool::preview() const {
  PreviewGeometry geometry;
  if (!anchor_.has_value()) {
    return geometry;
  }
  const std::optional<double> s2 = s2_.has_value() ? s2_ : cursor_s_;
  if (!s2.has_value()) {
    return geometry;
  }
  // Ghost one ring per instance the commit would place, expanded exactly as the
  // mesher does (span_preview_points shares make_span_repeat with the commit).
  const ObjectRepeat repeat = make_span_repeat(s1_, *s2, t_, distance_m_);
  for (const std::array<double, 2>& point :
       span_preview_points(document_.network(), *anchor_, repeat)) {
    geometry.add_handle(point[0], point[1], 0.0, HandleKind::Node, HandleState::Hovered);
  }
  return geometry;
}

QString PropSpanTool::instruction() const {
  return tr("Click two stations on one road to place a repeating prop span; Enter commits, "
            "[ / ] set spacing (%1 m), Backspace steps back, Esc cancels")
      .arg(distance_m_, 0, 'f', 1);
}

} // namespace roadmaker::editor
