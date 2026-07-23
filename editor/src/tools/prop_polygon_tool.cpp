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

#include "tools/prop_polygon_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/object.hpp"

#include <QRandomGenerator>
#include <QUndoStack>
#include <algorithm>
#include <utility>
#include <vector>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {

namespace {

/// Density keys nudge by this step, clamped to [min, max] props per 100 m² — a
/// usable spread from a sparse orchard to a dense thicket (mirrors the other
/// prop tools' [0.5, 50] clamp so the keys feel the same).
constexpr double kDensityStep = 0.5;
constexpr double kDensityMin = 0.5;
constexpr double kDensityMax = 50.0;

void append_segment(PreviewGeometry& geometry, double x0, double y0, double x1, double y1) {
  geometry.line_positions.insert(geometry.line_positions.end(), {x0, y0, 0.0, x1, y1, 0.0});
}

} // namespace

PropPolygonTool::PropPolygonTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void PropPolygonTool::set_params_provider(std::function<LibraryItem()> provider) {
  params_provider_ = std::move(provider);
}

LibraryItem PropPolygonTool::current_item() const {
  return params_provider_ ? params_provider_() : LibraryItem{};
}

void PropPolygonTool::set_seed(std::uint32_t seed) {
  seed_ = seed;
  invalidate();
}

void PropPolygonTool::invalidate() {
  dirty_ = true;
  distribution_.reset();
  emit preview_changed();
}

void PropPolygonTool::deactivate() {
  reset_session();
}

bool PropPolygonTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  if (vertices_.empty() && !is_prop_asset(current_item())) {
    emit toast_requested(tr("Select a tree or shrub in the Library first"), ToastSeverity::Warning);
    return true;
  }
  vertices_.push_back(Waypoint{.x = event.world_x, .y = event.world_y});
  invalidate();
  emit status_message(tr("%n region vertex(es) — Enter scatters props once the region is closed",
                         nullptr,
                         static_cast<int>(vertices_.size())));
  return true;
}

bool PropPolygonTool::mouse_move(const ToolEvent& event) {
  cursor_ = Waypoint{.x = event.world_x, .y = event.world_y};
  // A hover only moves the open outline edge; the cached scatter is NOT part of
  // the cursor, so no recompute here.
  emit preview_changed();
  return false; // hovering never consumes: camera nav and hover readout stay live
}

bool PropPolygonTool::mouse_double_click(const ToolEvent& event) {
  static_cast<void>(event); // the pair's first press already placed the vertex
  bake();
  return true;
}

bool PropPolygonTool::key_press(int key, Qt::KeyboardModifiers modifiers) {
  static_cast<void>(modifiers);
  if (key == Qt::Key_Return || key == Qt::Key_Enter) {
    bake();
    return true;
  }
  if (key == Qt::Key_BracketLeft) {
    adjust_density(-kDensityStep);
    return true;
  }
  if (key == Qt::Key_BracketRight) {
    adjust_density(kDensityStep);
    return true;
  }
  if (key == Qt::Key_R) {
    randomize();
    return true;
  }
  if (key == Qt::Key_Backspace) {
    if (vertices_.empty()) {
      return false;
    }
    vertices_.pop_back();
    invalidate();
    emit status_message(tr("Removed the last vertex"));
    return true;
  }
  if (key == Qt::Key_Escape) {
    if (vertices_.empty()) {
      return false;
    }
    reset_session();
    emit status_message(tr("Prop region cancelled"));
    return true;
  }
  return false;
}

void PropPolygonTool::adjust_density(double delta) {
  density_per_100m2_ = std::clamp(density_per_100m2_ + delta, kDensityMin, kDensityMax);
  invalidate();
  emit status_message(tr("Prop density %1 per 100 m²").arg(density_per_100m2_, 0, 'f', 1));
}

void PropPolygonTool::randomize() {
  // A fresh seed re-scatters the same region with the same density (GW-2). This
  // is deliberately a tool-internal key, not a global QAction shortcut.
  seed_ = QRandomGenerator::global()->generate();
  invalidate();
  emit status_message(tr("Re-scattered the props"));
}

void PropPolygonTool::ensure_distribution() const {
  if (!dirty_) {
    return;
  }
  dirty_ = false;
  distribution_.reset();
  if (vertices_.size() < 3) {
    return;
  }
  const LibraryItem item = current_item();
  if (!is_prop_asset(item)) {
    return;
  }
  Expected<PropCurveDistribution> distribution = distribute_props_in_polygon(
      document_.network(),
      vertices_,
      item,
      PropScatterParams{.density_per_100m2 = density_per_100m2_, .seed = seed_});
  if (distribution.has_value()) {
    distribution_ = std::move(*distribution);
  }
}

void PropPolygonTool::bake() {
  if (vertices_.size() < 3) {
    emit status_message(tr("A prop region needs at least three vertices"));
    return;
  }
  ensure_distribution();
  if (!distribution_.has_value() || distribution_->props.empty()) {
    // Session kept: the region is the user's work; move it or raise density.
    emit status_message(tr("No props landed on a road — move the region or raise the density"));
    return;
  }
  const std::size_t placed = distribution_->props.size();
  const std::size_t skipped = distribution_->skipped;

  // ONE undo unit: every scattered prop adds together (the prop-curve macro
  // pattern), so a single Ctrl+Z removes the whole bake.
  document_.undo_stack()->beginMacro(tr("Bake prop polygon"));
  for (auto& [road, object] : distribution_->props) {
    (void)document_.push_command(edit::add_object(document_.network(), road, std::move(object)));
  }
  document_.undo_stack()->endMacro();

  reset_session();
  if (skipped > 0) {
    emit status_message(
        tr("Baked %1 props (%2 skipped) — Ctrl+Z to undo").arg(placed).arg(skipped));
  } else {
    emit status_message(tr("Baked %1 props — Ctrl+Z to undo").arg(placed));
  }
}

void PropPolygonTool::reset_session() {
  vertices_.clear();
  cursor_.reset();
  invalidate();
}

PreviewGeometry PropPolygonTool::preview() const {
  PreviewGeometry geometry;

  for (const Waypoint& vertex : vertices_) {
    geometry.add_handle(vertex.x, vertex.y);
  }

  // The region outline: edges between placed vertices, an open edge to the
  // cursor, and (once a region is forming) a closing edge back to the first
  // vertex so the enclosed area reads at a glance.
  for (std::size_t i = 0; i + 1 < vertices_.size(); ++i) {
    append_segment(
        geometry, vertices_[i].x, vertices_[i].y, vertices_[i + 1].x, vertices_[i + 1].y);
  }
  if (!vertices_.empty() && cursor_.has_value()) {
    append_segment(geometry, vertices_.back().x, vertices_.back().y, cursor_->x, cursor_->y);
    if (vertices_.size() >= 2) {
      append_segment(geometry, cursor_->x, cursor_->y, vertices_.front().x, vertices_.front().y);
    }
  } else if (vertices_.size() >= 3) {
    append_segment(
        geometry, vertices_.back().x, vertices_.back().y, vertices_.front().x, vertices_.front().y);
  }

  // One ghost per scattered prop the bake will author (cached — see
  // ensure_distribution).
  ensure_distribution();
  if (distribution_.has_value()) {
    for (const std::array<double, 2>& point : distribution_->preview_points) {
      geometry.add_handle(point[0], point[1], 0.0, HandleKind::Node, HandleState::Hovered);
    }
  }

  return geometry;
}

QString PropPolygonTool::instruction() const {
  return tr("Click to outline a region; Enter scatters props, [ / ] set density (%1 / 100 m²), "
            "R re-scatters, Backspace undoes a vertex, Esc cancels")
      .arg(density_per_100m2_, 0, 'f', 1);
}

} // namespace roadmaker::editor
