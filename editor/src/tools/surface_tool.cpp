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

#include "tools/surface_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/mesh/surface_boundary.hpp"
#include "roadmaker/road/network.hpp"

#include <array>
#include <cmath>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// The tangent handle of `node` sits at the tip of its tangent vector. Very
/// short tangents would bury their handle inside the node knob, so the drawn
/// tip is pushed out to a minimum standoff along the tangent direction (or
/// along +X when the tangent is exactly zero, which is the only way to give a
/// zeroed tangent a handle the user can grab back).
constexpr double kTangentMinStandoff = 2.0;

std::array<double, 2> tangent_tip(const SurfaceNode& node, bool incoming) {
  const double tx = incoming ? -node.tangent_in_x : node.tangent_out_x;
  const double ty = incoming ? -node.tangent_in_y : node.tangent_out_y;
  const double length = std::hypot(tx, ty);
  if (length < kTangentMinStandoff) {
    const double ux = length > 0.0 ? tx / length : 1.0;
    const double uy = length > 0.0 ? ty / length : 0.0;
    return {node.x + (ux * kTangentMinStandoff), node.y + (uy * kTangentMinStandoff)};
  }
  return {node.x + tx, node.y + ty};
}

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

} // namespace

SurfaceTool::SurfaceTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void SurfaceTool::activate() {}

void SurfaceTool::deactivate() {
  if (drag_.has_value()) {
    document_.cancel_preview();
    drag_.reset();
  }
  active_.reset();
  emit preview_changed();
}

SurfaceId SurfaceTool::target() const {
  const std::vector<SurfaceId> surfaces = selection_.selected_surfaces();
  return surfaces.empty() ? SurfaceId{} : surfaces.back();
}

std::vector<SurfaceNode> SurfaceTool::nodes() const {
  // Mid-drag the network already carries the previewed loop, so reading it back
  // through the same query keeps the handles on the geometry being rendered.
  return surface_boundary_nodes(document_.network(), target());
}

std::optional<SurfaceTool::DragState> SurfaceTool::pick_grip(double x, double y) const {
  const SurfaceId surface = target();
  const std::vector<SurfaceNode> loop = nodes();
  if (!surface.is_valid() || loop.empty()) {
    return std::nullopt;
  }
  std::optional<DragState> best;
  double best_dist = pick_radius_;
  const auto consider = [&](std::size_t index, Grip grip, double px, double py) {
    const double dist = std::hypot(x - px, y - py);
    if (dist <= best_dist) {
      best_dist = dist;
      best = DragState{.surface = surface, .index = index, .grip = grip, .base = loop};
    }
  };
  // Tangent handles first: they sit on top of the node they belong to, so a
  // click in the overlap grabs the tangent (the node is reachable everywhere
  // else on its knob).
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const std::array<double, 2> out = tangent_tip(loop[i], /*incoming=*/false);
    consider(i, Grip::TangentOut, out[0], out[1]);
    const std::array<double, 2> in = tangent_tip(loop[i], /*incoming=*/true);
    consider(i, Grip::TangentIn, in[0], in[1]);
  }
  for (std::size_t i = 0; i < loop.size(); ++i) {
    consider(i, Grip::Node, loop[i].x, loop[i].y);
  }
  return best;
}

std::optional<std::size_t> SurfaceTool::pick_midpoint(double x, double y) const {
  const std::vector<SurfaceNode> loop = nodes();
  if (loop.size() < 3) {
    return std::nullopt;
  }
  std::optional<std::size_t> best;
  double best_dist = pick_radius_;
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const SurfaceNode& a = loop[i];
    const SurfaceNode& b = loop[(i + 1) % loop.size()];
    const double mx = 0.5 * (a.x + b.x);
    const double my = 0.5 * (a.y + b.y);
    const double dist = std::hypot(x - mx, y - my);
    if (dist <= best_dist) {
      best_dist = dist;
      best = i + 1; // insert AFTER node i
    }
  }
  return best;
}

std::vector<SurfaceNode> SurfaceTool::dragged_nodes(double x, double y) const {
  std::vector<SurfaceNode> loop = drag_->base;
  SurfaceNode& node = loop[drag_->index];
  switch (drag_->grip) {
  case Grip::Node: {
    // Moving a node carries its tangents along unchanged — the shape follows
    // the hand instead of shearing around the grabbed point.
    node.x = x;
    node.y = y;
    break;
  }
  case Grip::TangentOut:
    node.tangent_out_x = x - node.x;
    node.tangent_out_y = y - node.y;
    break;
  case Grip::TangentIn:
    // The incoming handle is drawn on the far side, so dragging it there means
    // negating: the stored tangent still points along the direction of travel.
    node.tangent_in_x = node.x - x;
    node.tangent_in_y = node.y - y;
    break;
  }
  return loop;
}

bool SurfaceTool::push(std::vector<SurfaceNode> nodes, const QString& success) {
  const Expected<void> pushed = document_.push_command(
      edit::set_surface_boundary(document_.network(), target(), std::move(nodes)));
  if (!pushed.has_value()) {
    emit status_message(
        tr("Cannot edit boundary: %1").arg(QString::fromStdString(pushed.error().message)));
    return false;
  }
  emit status_message(success);
  emit preview_changed();
  return true;
}

bool SurfaceTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton) || drag_.has_value()) {
    return false;
  }

  if (auto grab = pick_grip(event.world_x, event.world_y)) {
    active_ = grab->index;
    drag_ = std::move(grab);
    emit preview_changed();
    return true;
  }

  if (const auto insert_at = pick_midpoint(event.world_x, event.world_y)) {
    insert_node(*insert_at);
    return true;
  }

  // Nothing grabbed: fall back to selecting whatever the cursor is over, so
  // the tool can reach a second surface without switching to Select.
  if (event.pick.has_value()) {
    selection_.select({.road = event.pick->road,
                       .lane = event.pick->lane,
                       .junction = event.pick->junction,
                       .surface = event.pick->surface});
  } else {
    selection_.clear();
  }
  active_.reset();
  emit preview_changed();
  return true;
}

bool SurfaceTool::mouse_move(const ToolEvent& event) {
  if (!drag_.has_value()) {
    return false;
  }
  std::vector<SurfaceNode> edited = dragged_nodes(event.world_x, event.world_y);
  const SurfaceId surface = drag_->surface;
  const auto factory = [surface, edited](const RoadNetwork& network) {
    return edit::set_surface_boundary(network, surface, edited);
  };
  // The first move starts the session; later ones replace it. A rejected
  // frame (a self-crossing loop mid-drag) simply leaves the last good one on
  // screen — the drag keeps going and can recover.
  const Expected<void> applied = drag_->moved
                                     ? document_.update_preview(factory)
                                     : document_.begin_preview(factory(document_.network()));
  if (applied.has_value()) {
    drag_->moved = true;
  }
  emit preview_changed();
  return true;
}

bool SurfaceTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!drag_.has_value()) {
    return false;
  }
  const bool moved = document_.preview_active();
  document_.commit_preview();
  drag_.reset();
  if (moved) {
    emit status_message(tr("Surface boundary edited"));
  }
  emit preview_changed();
  return true;
}

void SurfaceTool::insert_node(std::size_t before_index) {
  std::vector<SurfaceNode> loop = nodes();
  if (loop.size() < 3 || before_index > loop.size()) {
    return;
  }
  const SurfaceNode& a = loop[before_index - 1];
  const SurfaceNode& b = loop[before_index % loop.size()];
  // Seeded on the chord with a Catmull-Rom-scale tangent, so the inserted node
  // starts on the shape the user clicked rather than snapping it flat.
  const double tx = 0.25 * (b.x - a.x);
  const double ty = 0.25 * (b.y - a.y);
  const SurfaceNode fresh{.x = 0.5 * (a.x + b.x),
                          .y = 0.5 * (a.y + b.y),
                          .tangent_in_x = tx,
                          .tangent_in_y = ty,
                          .tangent_out_x = tx,
                          .tangent_out_y = ty};
  loop.insert(loop.begin() + static_cast<std::ptrdiff_t>(before_index), fresh);
  const std::vector<SurfaceNode> inserted = loop;
  if (!push(std::move(loop), tr("Node inserted — drag to place it"))) {
    return;
  }
  // Grab the fresh node so the same gesture can keep dragging it (that drag is
  // then a second undo entry, per §3's one-command-per-gesture).
  active_ = before_index;
  drag_ = DragState{.surface = target(),
                    .index = before_index,
                    .grip = Grip::Node,
                    .base = inserted,
                    .moved = false};
}

void SurfaceTool::delete_active_node() {
  std::vector<SurfaceNode> loop = nodes();
  if (!active_.has_value() || *active_ >= loop.size()) {
    return;
  }
  if (loop.size() <= 3) {
    emit status_message(tr("A surface boundary needs at least 3 nodes"));
    return;
  }
  loop.erase(loop.begin() + static_cast<std::ptrdiff_t>(*active_));
  if (push(std::move(loop), tr("Node deleted"))) {
    active_.reset();
  }
}

void SurfaceTool::revert_to_derived() {
  const Expected<void> pushed =
      document_.push_command(edit::revert_surface_to_derived(document_.network(), target()));
  if (!pushed.has_value()) {
    emit status_message(
        tr("Cannot revert surface: %1").arg(QString::fromStdString(pushed.error().message)));
    return;
  }
  active_.reset();
  emit status_message(tr("Surface reverted to derived"));
  emit preview_changed();
}

bool SurfaceTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Escape) {
    if (drag_.has_value()) {
      document_.cancel_preview();
      drag_.reset();
      emit status_message(tr("Boundary edit cancelled"));
      emit preview_changed();
      return true;
    }
    if (active_.has_value()) {
      active_.reset();
      emit preview_changed();
      return true;
    }
    return false;
  }
  if ((key == Qt::Key_Delete || key == Qt::Key_Backspace) && active_.has_value() &&
      !drag_.has_value()) {
    delete_active_node();
    return true;
  }
  return false;
}

PreviewGeometry SurfaceTool::preview() const {
  PreviewGeometry geometry;
  const std::vector<SurfaceNode> loop = nodes();
  if (loop.size() < 3) {
    return geometry;
  }

  // The boundary itself, as the tool's own tessellation of it — what the mesher
  // will fill, so a bulging tangent reads before the surface re-meshes.
  const std::vector<std::array<double, 2>> ring = sample_surface_boundary(loop);
  for (std::size_t i = 0; i < ring.size(); ++i) {
    const std::array<double, 2>& a = ring[i];
    const std::array<double, 2>& b = ring[(i + 1) % ring.size()];
    append_segment(geometry, a[0], a[1], b[0], b[1]);
  }

  for (std::size_t i = 0; i < loop.size(); ++i) {
    const bool grabbed = drag_.has_value() && drag_->index == i && drag_->grip == Grip::Node;
    const HandleState state = grabbed          ? HandleState::Grabbed
                              : (active_ == i) ? HandleState::Hovered
                                               : HandleState::Idle;
    geometry.add_handle(loop[i].x, loop[i].y, 0.0, HandleKind::Node, state);

    // Tangent whiskers with a grabbable knob at each tip.
    for (const bool incoming : {false, true}) {
      const std::array<double, 2> tip = tangent_tip(loop[i], incoming);
      append_segment(geometry, loop[i].x, loop[i].y, tip[0], tip[1]);
      const Grip grip = incoming ? Grip::TangentIn : Grip::TangentOut;
      const bool held = drag_.has_value() && drag_->index == i && drag_->grip == grip;
      geometry.add_handle(tip[0],
                          tip[1],
                          0.0,
                          HandleKind::Midpoint,
                          held ? HandleState::Grabbed : HandleState::Idle);
    }
  }

  // Segment midpoints: click to insert a node there.
  for (std::size_t i = 0; i < loop.size(); ++i) {
    const SurfaceNode& a = loop[i];
    const SurfaceNode& b = loop[(i + 1) % loop.size()];
    geometry.add_handle(
        0.5 * (a.x + b.x), 0.5 * (a.y + b.y), 0.0, HandleKind::Sample, HandleState::Idle);
  }

  return geometry;
}

QString SurfaceTool::instruction() const {
  if (!target().is_valid()) {
    return tr("Click a ground surface to edit its boundary");
  }
  return tr("Drag a node to reshape the boundary · drag a tangent tip to curve it · click a "
            "midpoint to insert a node · click a node then Delete removes it · Esc cancels");
}

} // namespace roadmaker::editor
