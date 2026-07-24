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

#include "tools/terrain_brush_tool.hpp"

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/road/network.hpp"
#include "roadmaker/road/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "document/document.hpp"

namespace roadmaker::editor {

namespace {

constexpr double kMinRadius = 1.0;
constexpr double kMinStrength = 0.01;
/// How far (as a fraction of the radius) the cursor must travel before the next
/// dab lands — dense enough for a smooth ridge, sparse enough to bound the
/// stroke's replay cost.
constexpr double kDabSpacingFraction = 0.25;

} // namespace

TerrainBrushTool::TerrainBrushTool(Document& document, QObject* parent)
    : Tool(parent), document_(document) {}

void TerrainBrushTool::activate() {
  stroking_ = false;
  stamps_.clear();
  have_cursor_ = false;
  emit preview_changed();
}

void TerrainBrushTool::deactivate() {
  // A tool switch mid-stroke abandons the drag cleanly.
  if (document_.preview_active()) {
    document_.cancel_preview();
  }
  stroking_ = false;
  stamps_.clear();
  have_cursor_ = false;
  emit preview_changed();
}

void TerrainBrushTool::set_radius(double radius) {
  radius_ = std::max(kMinRadius, radius);
  emit preview_changed();
}

void TerrainBrushTool::set_strength(double strength) {
  strength_ = std::max(kMinStrength, strength);
}

void TerrainBrushTool::set_mode(BrushMode mode) {
  mode_ = mode;
}

void TerrainBrushTool::add_stamp(double x, double y) {
  stamps_.push_back(BrushStamp{
      .center_x = x, .center_y = y, .radius = radius_, .strength = strength_, .mode = mode_});
}

void TerrainBrushTool::apply_stroke() {
  const auto factory = [this](const RoadNetwork& network) -> std::unique_ptr<edit::Command> {
    Expected<std::unique_ptr<edit::Command>> built = edit::stamp_terrain(network, stamps_);
    return built.has_value() ? std::move(*built) : nullptr;
  };
  if (document_.preview_active()) {
    static_cast<void>(document_.update_preview(factory));
  } else if (std::unique_ptr<edit::Command> command = factory(document_.network())) {
    static_cast<void>(document_.begin_preview(std::move(command)));
  }
}

bool TerrainBrushTool::mouse_press(const ToolEvent& event) {
  if (!(event.buttons & Qt::LeftButton)) {
    return false;
  }
  have_cursor_ = true;
  cursor_x_ = event.world_x;
  cursor_y_ = event.world_y;
  if (document_.network().terrain().empty()) {
    emit toast_requested(tr("Create a terrain field first (Edit ▸ Terrain ▸ Create Terrain Field)"),
                         ToastSeverity::Warning);
    return true;
  }
  stroking_ = true;
  stamps_.clear();
  add_stamp(event.world_x, event.world_y);
  apply_stroke();
  emit preview_changed();
  return true;
}

bool TerrainBrushTool::mouse_move(const ToolEvent& event) {
  have_cursor_ = true;
  cursor_x_ = event.world_x;
  cursor_y_ = event.world_y;
  if (!stroking_) {
    // Not painting: just keep the brush ring under the cursor. Don't consume,
    // so viewport hover/navigation still work.
    emit preview_changed();
    return false;
  }
  // Space dabs along the path so a slow drag does not accumulate thousands.
  const BrushStamp& last = stamps_.back();
  const double dx = event.world_x - last.center_x;
  const double dy = event.world_y - last.center_y;
  if (std::sqrt((dx * dx) + (dy * dy)) >= radius_ * kDabSpacingFraction) {
    add_stamp(event.world_x, event.world_y);
    apply_stroke();
  }
  emit preview_changed();
  return true;
}

bool TerrainBrushTool::mouse_release(const ToolEvent& event) {
  static_cast<void>(event);
  if (!stroking_) {
    return false;
  }
  stroking_ = false;
  const bool committed = document_.preview_active();
  document_.commit_preview();
  stamps_.clear();
  if (committed) {
    emit status_message(tr("Terrain sculpted"));
  }
  emit preview_changed();
  return true;
}

PreviewGeometry TerrainBrushTool::preview() const {
  PreviewGeometry geometry;
  if (!have_cursor_) {
    return geometry;
  }
  // A ring hugging the ground at the brush radius. Sampling the field height at
  // each vertex keeps it on the sculpted surface rather than punching through a
  // raised hill; on an absent field sample_height returns 0 (flat plane).
  const HeightField& field = document_.network().terrain();
  constexpr int kSegments = 48;
  double prev_x = 0.0;
  double prev_y = 0.0;
  double prev_z = 0.0;
  for (int i = 0; i <= kSegments; ++i) {
    const double angle = (2.0 * M_PI * static_cast<double>(i)) / static_cast<double>(kSegments);
    const double x = cursor_x_ + (radius_ * std::cos(angle));
    const double y = cursor_y_ + (radius_ * std::sin(angle));
    const double z = sample_height(field, x, y);
    if (i > 0) {
      geometry.line_positions.insert(geometry.line_positions.end(),
                                     {prev_x, prev_y, prev_z, x, y, z});
    }
    prev_x = x;
    prev_y = y;
    prev_z = z;
  }
  return geometry;
}

QString TerrainBrushTool::instruction() const {
  switch (mode_) {
  case BrushMode::Raise:
    return tr("Drag to raise the ground · adjust radius/strength/mode in the options row");
  case BrushMode::Lower:
    return tr("Drag to lower the ground · adjust radius/strength/mode in the options row");
  case BrushMode::Smooth:
    return tr("Drag to smooth the ground · adjust radius/strength/mode in the options row");
  }
  return {};
}

} // namespace roadmaker::editor
