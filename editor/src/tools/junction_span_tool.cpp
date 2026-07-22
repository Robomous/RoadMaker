// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "tools/junction_span_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/road.hpp"
#include "roadmaker/tol.hpp"

#include <algorithm>
#include <cmath>
#include <span>

#include "document/document.hpp"
#include "document/selection_model.hpp"
#include "viewport/picking.hpp"

namespace roadmaker::editor {

namespace {

/// How many segments a staged span is drawn with — enough that a span over a
/// curve reads as following the reference line rather than cutting across it.
constexpr int kSpanPreviewSegments = 16;

} // namespace

JunctionSpanTool::JunctionSpanTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {
  // A load replaces the whole network, so every RoadId staged here goes stale
  // (and can alias a fresh road) — drop the session with it.
  connect(&document_, &Document::loaded, this, [this] { reset_session(); });
}

void JunctionSpanTool::activate() {
  reset_session();
}

void JunctionSpanTool::deactivate() {
  reset_session();
}

void JunctionSpanTool::reset_session() {
  spans_.clear();
  drag_road_.reset();
  drag_s0_ = 0.0;
  drag_s1_ = 0.0;
  refused_hover_ = RoadId{};
  emit preview_changed();
}

std::optional<RoadStation> JunctionSpanTool::road_under(const ToolEvent& event) const {
  if (event.pick.has_value() && event.pick->road.is_valid()) {
    if (const Road* picked = document_.network().road(event.pick->road);
        picked != nullptr && !picked->plan_view.empty()) {
      const StationCoord at = find_station(picked->plan_view, event.world_x, event.world_y);
      return RoadStation{
          .road = event.pick->road, .s = std::clamp(at.s, 0.0, picked->length), .t = at.t};
    }
  }
  return nearest_road_station(document_.network(), event.world_x, event.world_y, snap_threshold_);
}

std::optional<double>
JunctionSpanTool::station_on(RoadId road_id, double world_x, double world_y) const {
  const Road* road = document_.network().road(road_id);
  if (road == nullptr || road->plan_view.empty()) {
    return std::nullopt;
  }
  // The drag runs ALONG the road, so lateral travel is ignored and the station
  // is clamped to the road — a drag that overshoots the end still stages a
  // valid interval instead of dying.
  return std::clamp(find_station(road->plan_view, world_x, world_y).s, 0.0, road->length);
}

bool JunctionSpanTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || drag_road_.has_value()) {
    return false;
  }
  const std::optional<RoadStation> hit = road_under(event);
  if (!hit.has_value()) {
    emit status_message(tr("Drag along a road to mark the stretch the junction covers"));
    return false; // nothing here — let the viewport keep the click
  }
  const Road* road = document_.network().road(hit->road);
  if (road == nullptr) {
    return false;
  }
  // §12.7: a virtual junction spans an uninterrupted THROUGH road, never
  // junction internals. The kernel refuses this too; refusing here keeps the
  // gesture from ending in a failed command.
  if (road->junction.is_valid()) {
    emit toast_requested(tr("A span junction covers a through road, not a junction's connecting "
                            "road"),
                         ToastSeverity::Warning);
    return true;
  }
  const bool already_staged =
      std::ranges::any_of(spans_, [&](const SpanArm& span) { return span.road == hit->road; });
  if (!already_staged && spans_.size() >= 2) {
    emit toast_requested(
        tr("A span junction covers one road, or two parallel roads — Enter commits, Esc resets"),
        ToastSeverity::Warning);
    return true;
  }

  drag_road_ = hit->road;
  drag_s0_ = hit->s;
  drag_s1_ = hit->s;
  emit status_message(tr("Drag to the far end of the stretch, then release"));
  emit preview_changed();
  return true;
}

bool JunctionSpanTool::mouse_move(const ToolEvent& event) {
  if (drag_road_.has_value()) {
    if (const std::optional<double> s = station_on(*drag_road_, event.world_x, event.world_y)) {
      drag_s1_ = *s;
      emit preview_changed();
    }
    return true;
  }

  // Hover cue: say why a connecting road cannot be spanned BEFORE the click,
  // rather than letting a refused command explain it afterwards.
  const std::optional<RoadStation> hit = road_under(event);
  const Road* road = hit.has_value() ? document_.network().road(hit->road) : nullptr;
  const bool refused = road != nullptr && road->junction.is_valid();
  const RoadId now = refused ? hit->road : RoadId{};
  if (now != refused_hover_) {
    refused_hover_ = now;
    if (refused) {
      emit status_message(tr("Connecting roads are junction internals — span a through road"));
    }
  }
  return false; // hovering never consumes: camera nav and the readout stay live
}

bool JunctionSpanTool::mouse_release(const ToolEvent& event) {
  if (!drag_road_.has_value()) {
    return false;
  }
  if (const std::optional<double> s = station_on(*drag_road_, event.world_x, event.world_y)) {
    drag_s1_ = *s;
  }
  const RoadId road = *drag_road_;
  const double s0 = std::min(drag_s0_, drag_s1_);
  const double s1 = std::max(drag_s0_, drag_s1_);
  drag_road_.reset();
  stage(road, s0, s1);
  return true;
}

void JunctionSpanTool::stage(RoadId road, double s0, double s1) {
  // create_span_junction wants a real forward interval; a click that never
  // travelled is a mis-gesture, not an error worth a command.
  if (s1 - s0 <= tol::kLength) {
    emit status_message(tr("Drag along the road — a span needs a length"));
    emit preview_changed();
    return;
  }
  const auto existing =
      std::ranges::find_if(spans_, [&](const SpanArm& span) { return span.road == road; });
  const SpanArm span{.road = road, .s_start = s0, .s_end = s1};
  if (existing != spans_.end()) {
    *existing = span; // re-dragging a staged road replaces its span
  } else {
    spans_.push_back(span);
  }
  emit status_message(spans_.size() == 1
                          ? tr("Span staged — drag a parallel road for a second span, or press "
                               "Enter to create the junction")
                          : tr("Two spans staged — press Enter to create the junction"));
  emit preview_changed();
}

bool JunctionSpanTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    commit();
    return true;
  }
  if (key == Qt::Key_Escape) {
    if (spans_.empty() && !drag_road_.has_value()) {
      return false;
    }
    reset_session();
    emit status_message(tr("Span junction cancelled"));
    return true;
  }
  return false;
}

void JunctionSpanTool::commit() {
  if (spans_.empty()) {
    emit status_message(tr("Drag along a road first — a span junction needs at least one span"));
    return;
  }
  // ONE command for the whole gesture: a span junction is a single record, so
  // there is nothing to wrap in a macro.
  const std::vector<SpanArm> spans = spans_;
  if (!document_
           .push_command(
               edit::create_span_junction(document_.network(), std::span<const SpanArm>(spans)))
           .has_value()) {
    emit toast_requested(tr("Could not create the span junction"), ToastSeverity::Warning);
    return;
  }
  // The junction id only exists after apply(); the creator path reports it in
  // the dirty set, which Document keeps by value for exactly this reason.
  for (const JunctionId created : document_.last_dirty().junctions) {
    const Junction* junction = document_.network().junction(created);
    if (junction != nullptr && !junction->spans.empty()) {
      selection_.select(SelectionEntry{.junction = created}, SelectMode::Replace);
      break;
    }
  }
  const std::size_t count = spans.size();
  reset_session();
  emit status_message(count == 1 ? tr("Span junction created — Ctrl+Z to undo")
                                 : tr("Span junction created over 2 roads — Ctrl+Z to undo"));
}

PreviewGeometry JunctionSpanTool::preview() const {
  PreviewGeometry geometry;

  const auto band = [&](RoadId road_id, double s0, double s1, HandleState state) {
    const Road* road = document_.network().road(road_id);
    if (road == nullptr || road->plan_view.empty()) {
      return;
    }
    const double lo = std::clamp(std::min(s0, s1), 0.0, road->length);
    const double hi = std::clamp(std::max(s0, s1), 0.0, road->length);
    const double step = (hi - lo) / kSpanPreviewSegments;
    PathPoint previous = road->plan_view.evaluate(lo);
    for (int i = 1; i <= kSpanPreviewSegments; ++i) {
      const PathPoint point = road->plan_view.evaluate(lo + (step * i));
      geometry.line_positions.insert(geometry.line_positions.end(),
                                     {previous.x, previous.y, 0.0, point.x, point.y, 0.0});
      previous = point;
    }
    const PathPoint start = road->plan_view.evaluate(lo);
    const PathPoint end = road->plan_view.evaluate(hi);
    geometry.add_handle(start.x, start.y, 0.0, HandleKind::Node, state);
    geometry.add_handle(end.x, end.y, 0.0, HandleKind::Node, state);
  };

  for (const SpanArm& span : spans_) {
    band(span.road, span.s_start, span.s_end, HandleState::Hovered);
  }
  if (drag_road_.has_value()) {
    band(*drag_road_, drag_s0_, drag_s1_, HandleState::Grabbed);
  }
  return geometry;
}

QString JunctionSpanTool::instruction() const {
  return tr("Drag along a road to mark the stretch a virtual junction covers; drag a parallel road "
            "for a second span · Enter creates it · Esc resets");
}

} // namespace roadmaker::editor
