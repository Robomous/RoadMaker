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

#pragma once

// Terrain Brush tool (p5-s4, issue #234, GW-2 step 9). Drag over the ground to
// sculpt the ONE scene height field: raise/lower push it up/down, smooth relaxes
// it. A whole press-drag-release stroke is ONE preview session that re-meshes
// terrain live every frame, committed as ONE undo entry (edit::stamp_terrain) on
// release — the surface-tool drag pattern. The brush math is the kernel's
// (apply_brush_stamp); the tool only spaces dabs along the cursor path and draws
// the brush ring. A stroke needs a terrain field to exist first (Edit ▸ Terrain
// ▸ Create Terrain Field, or a DEM import) — with none it toasts and no-ops.

#include "roadmaker/road/terrain_brush.hpp"

#include <QObject>
#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

class TerrainBrushTool : public Tool {
  Q_OBJECT

public:
  explicit TerrainBrushTool(Document& document, QObject* parent = nullptr);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;

  [[nodiscard]] PreviewGeometry preview() const override;
  [[nodiscard]] QString instruction() const override;

  /// Brush options, driven by the Tool Options row. Radius/strength are clamped
  /// to sane minimums; the mode picks raise/lower/smooth.
  void set_radius(double radius);
  void set_strength(double strength);
  void set_mode(BrushMode mode);

  [[nodiscard]] double radius() const { return radius_; }

  [[nodiscard]] double strength() const { return strength_; }

  [[nodiscard]] BrushMode mode() const { return mode_; }

  [[nodiscard]] bool stroking() const { return stroking_; }

private:
  /// Builds/updates the preview command from the stamps gathered so far, against
  /// the base network (the preview factory reverts to base each frame). A no-op
  /// stroke (all dabs off-grid) simply leaves no session active.
  void apply_stroke();

  /// Appends a dab at (x, y) with the current options.
  void add_stamp(double x, double y);

  Document& document_;

  double radius_ = 20.0;
  double strength_ = 0.5;
  BrushMode mode_ = BrushMode::Raise;

  bool stroking_ = false;          ///< the left button is down over a field
  std::vector<BrushStamp> stamps_; ///< the current stroke's dabs, in order

  bool have_cursor_ = false; ///< draw the brush ring once the cursor is known
  double cursor_x_ = 0.0;
  double cursor_y_ = 0.0;
};

} // namespace roadmaker::editor
