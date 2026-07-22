// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#include "tools/create_junction_tool.hpp"

#include "roadmaker/edit/connection.hpp"
#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"

#include <algorithm>
#include <cmath>

#include "document/document.hpp"

namespace roadmaker::editor {

CreateJunctionTool::CreateJunctionTool(Document& document, QObject* parent)
    : Tool(parent), document_(document) {}

void CreateJunctionTool::activate() {
  reset_session();
}

void CreateJunctionTool::deactivate() {
  reset_session();
}

std::optional<RoadEnd> CreateJunctionTool::snap_end(const Waypoint& cursor) const {
  // Endpoints only: the tool selects road ends, never tangent rays or grid.
  edit::SnapOptions options = snap_options_;
  options.endpoints = true;
  options.tangent = false;
  options.grid = std::nullopt;
  const auto snap = edit::snap_point(document_.network(), cursor, options);
  if (!snap.has_value() || snap->kind != edit::SnapKind::RoadEndpoint || !snap->road.has_value()) {
    return std::nullopt;
  }
  const Road* road = document_.network().road(*snap->road);
  if (road == nullptr) {
    return std::nullopt;
  }
  // Resolve which end the snap landed on by proximity to the two endpoints.
  const auto start = road->plan_view.evaluate(0.0);
  const auto end = road->plan_view.evaluate(road->plan_view.length());
  const double to_start = std::hypot(snap->position.x - start.x, snap->position.y - start.y);
  const double to_end = std::hypot(snap->position.x - end.x, snap->position.y - end.y);
  return RoadEnd{.road = *snap->road,
                 .contact = to_start <= to_end ? ContactPoint::Start : ContactPoint::End};
}

std::optional<edit::SideSnap> CreateJunctionTool::snap_side(const Waypoint& cursor) const {
  if (ends_.size() != 1) {
    return std::nullopt; // the tee needs exactly one attaching end
  }
  edit::SnapOptions options = snap_options_;
  // A body point of a road whose end is already selected is not a tee
  // target — that end IS the junction arm.
  options.exclude_road = ends_.front().road;
  return edit::snap_to_road_side(document_.network(), cursor, options);
}

void CreateJunctionTool::toggle(const RoadEnd& end) {
  const auto it = std::ranges::find(ends_, end);
  if (it != ends_.end()) {
    ends_.erase(it); // clicking a selected end again deselects it
  } else {
    ends_.push_back(end);
  }
}

bool CreateJunctionTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  // LMB belongs to the tool even on a miss (M2 button map); a hit near a road
  // end toggles it into the selection. Endpoint snapping wins over the side
  // anchor inside its own radius — clicking an end must never read as a tee.
  const Waypoint cursor{event.world_x, event.world_y};
  if (const auto end = snap_end(cursor); end.has_value()) {
    side_anchor_.reset();
    toggle(*end);
    emit preview_changed();
    emit_count_status();
    return true;
  }
  if (const auto side = snap_side(cursor); side.has_value()) {
    side_anchor_ = side;
    emit preview_changed();
    const Road* target = document_.network().road(side->road);
    const double gap = edit::t_attach_gap(document_.network(), ends_.front(), side->road, side->s);
    emit status_message(tr("Tee into %1 at s=%2 m — replaces s=%3–%4 m; Enter attaches, "
                           "Esc cancels")
                            .arg(QString::fromStdString(target->name))
                            .arg(side->s, 0, 'f', 1)
                            .arg(side->s - gap, 0, 'f', 1)
                            .arg(side->s + gap, 0, 'f', 1));
  }
  return true;
}

bool CreateJunctionTool::mouse_move(const ToolEvent& event) {
  hover_ = Waypoint{event.world_x, event.world_y};
  emit preview_changed();
  return false; // hover never consumes: camera/other handlers still see it
}

bool CreateJunctionTool::key_press(int key, Qt::KeyboardModifiers /*modifiers*/) {
  if (key == Qt::Key_Escape) {
    reset_session();
    emit preview_changed();
    emit status_message(tr("Create Junction — selection cleared"));
    return true;
  }
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    if (side_anchor_.has_value()) {
      generate_t_attach();
    } else {
      generate();
    }
    return true;
  }
  return false;
}

void CreateJunctionTool::generate_t_attach() {
  if (ends_.size() != 1 || !side_anchor_.has_value()) {
    emit status_message(tr("Tee needs exactly 1 selected road end plus a road-side anchor"));
    return;
  }
  // ONE undo step: attach_t_junction composes split + delete + junction.
  if (!document_.push_command(edit::attach_t_junction(
          document_.network(), ends_.front(), side_anchor_->road, side_anchor_->s))) {
    return; // push_command surfaced the failure in Diagnostics
  }
  reset_session();
  emit preview_changed();
  emit status_message(tr("T-junction attached"));
}

void CreateJunctionTool::generate() {
  if (ends_.size() < 2) {
    emit status_message(tr("Select at least 2 road ends before generating"));
    return;
  }
  // Idempotency (finding 5): if this exact arm set already forms a junction,
  // regenerate it in place instead of overlaying a duplicate.
  if (const auto existing = edit::matching_junction(document_.network(), ends_)) {
    const roadmaker::Junction* junction = document_.network().junction(*existing);
    const QString id =
        junction != nullptr ? QString::fromStdString(junction->odr_id) : QStringLiteral("?");
    if (document_.push_command(edit::regenerate_junction(document_.network(), *existing))) {
      emit toast_requested(tr("Selection matches junction %1 — regenerated in place").arg(id),
                           ToastSeverity::Info);
    }
    reset_session();
    emit preview_changed();
    return;
  }
  // Partial overlap: an end already belongs to a junction. create_junction
  // would refuse (the single-owner invariant); surface it as a warning rather
  // than a silent diagnostic, and don't push a doomed command.
  for (const RoadEnd& end : ends_) {
    if (edit::junction_at_end(document_.network(), end).has_value()) {
      emit toast_requested(
          tr("A selected end already belongs to a junction — regenerate that one instead"),
          ToastSeverity::Warning);
      reset_session();
      emit preview_changed();
      return;
    }
  }
  // Preview first so the status bar can report dropped turns the generator
  // omits (nearly-parallel arms whose fit would loop).
  const auto preview = edit::preview_junction(document_.network(), ends_);
  if (!preview.has_value()) {
    emit status_message(
        tr("Cannot create junction: %1").arg(QString::fromStdString(preview.error().message)));
    return;
  }
  if (!document_.push_command(edit::create_junction(document_.network(), ends_))) {
    return; // push_command already logged and surfaced the failure
  }
  QString message = tr("Created junction with %1 connection(s)").arg(preview->connection_count);
  if (!preview->dropped_turns.empty()) {
    message += tr(" — %1 turn(s) dropped").arg(static_cast<int>(preview->dropped_turns.size()));
  }
  reset_session();
  emit preview_changed();
  emit status_message(message);
}

PreviewGeometry CreateJunctionTool::preview() const {
  PreviewGeometry geometry;
  const auto add_point = [&](double x, double y) { geometry.add_handle(x, y); };
  for (const RoadEnd& end : ends_) {
    const Road* road = document_.network().road(end.road);
    if (road == nullptr) {
      continue;
    }
    const auto pose = road->plan_view.evaluate(
        end.contact == ContactPoint::Start ? 0.0 : road->plan_view.length());
    add_point(pose.x, pose.y);
  }
  if (side_anchor_.has_value()) {
    add_tee_preview(geometry, *side_anchor_);
  }
  if (hover_.has_value()) {
    if (const auto end = snap_end(*hover_); end.has_value()) {
      const Road* road = document_.network().road(end->road);
      const auto pose = road->plan_view.evaluate(
          end->contact == ContactPoint::Start ? 0.0 : road->plan_view.length());
      add_point(pose.x, pose.y);
    } else if (const auto side = snap_side(*hover_); side.has_value()) {
      // Live tee preview while hovering, same as a placed anchor.
      add_tee_preview(geometry, *side);
    }
  }
  return geometry;
}

/// The tee overlay (discoverability package, issue #103): the anchor marker
/// on the target's reference line, a dashed ghost line from the selected end
/// to the anchor, and the [s−gap, s+gap] span the attach will replace —
/// highlighted along the reference line so the user sees exactly what
/// generation consumes. Lines are xyz pairs; the dash pattern is emitted as
/// separate segments (the viewport draws PreviewGeometry lines pairwise).
void CreateJunctionTool::add_tee_preview(PreviewGeometry& geometry,
                                         const edit::SideSnap& side) const {
  const RoadNetwork& network = document_.network();
  const Road* target = network.road(side.road);
  if (target == nullptr || ends_.size() != 1) {
    return;
  }
  const auto add_segment = [&](double ax, double ay, double bx, double by) {
    geometry.line_positions.insert(geometry.line_positions.end(), {ax, ay, 0.0, bx, by, 0.0});
  };

  // Anchor marker on the reference line.
  geometry.add_handle(
      side.position.x, side.position.y, 0.0, HandleKind::Node, HandleState::Hovered);

  // Dashed ghost from the attaching end to the anchor (1 m dash / 1 m gap).
  const Road* branch = network.road(ends_.front().road);
  if (branch != nullptr) {
    const auto from = branch->plan_view.evaluate(
        ends_.front().contact == ContactPoint::Start ? 0.0 : branch->plan_view.length());
    const double dx = side.position.x - from.x;
    const double dy = side.position.y - from.y;
    const double length = std::hypot(dx, dy);
    constexpr double kDash = 1.0;
    for (double d = 0.0; d < length; d += 2.0 * kDash) {
      const double e = std::min(d + kDash, length);
      add_segment(from.x + (dx * d / length),
                  from.y + (dy * d / length),
                  from.x + (dx * e / length),
                  from.y + (dy * e / length));
    }
  }

  // The replaced span [s−gap, s+gap], drawn along the reference line.
  const double gap = edit::t_attach_gap(network, ends_.front(), side.road, side.s);
  if (gap <= 0.0) {
    return;
  }
  const double s0 = std::max(0.0, side.s - gap);
  const double s1 = std::min(target->plan_view.length(), side.s + gap);
  const int steps = std::max(2, static_cast<int>((s1 - s0)));
  auto prev = target->plan_view.evaluate(s0);
  for (int i = 1; i <= steps; ++i) {
    const auto next = target->plan_view.evaluate(s0 + ((s1 - s0) * static_cast<double>(i) / steps));
    add_segment(prev.x, prev.y, next.x, next.y);
    prev = next;
  }
  // Span end ticks: 3 m perpendicular strokes at the future cut faces.
  constexpr double kTickHalf = 3.0;
  for (const double s_cut : {s0, s1}) {
    const auto pose = target->plan_view.evaluate(s_cut);
    add_segment(pose.x + (kTickHalf * std::sin(pose.hdg)),
                pose.y - (kTickHalf * std::cos(pose.hdg)),
                pose.x - (kTickHalf * std::sin(pose.hdg)),
                pose.y + (kTickHalf * std::cos(pose.hdg)));
  }
}

void CreateJunctionTool::reset_session() {
  ends_.clear();
  side_anchor_.reset();
  hover_.reset();
}

void CreateJunctionTool::emit_count_status() {
  emit status_message(tr("%1 road end(s) selected — Enter generates, Esc cancels")
                          .arg(static_cast<int>(ends_.size())));
}

QString CreateJunctionTool::instruction() const {
  return tr("Click 2+ road ends, or one end and a road body to tee into it · Enter generates · Esc "
            "cancels");
}

} // namespace roadmaker::editor
