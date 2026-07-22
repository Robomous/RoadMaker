// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Prop Curve tool (p6-s4, issue #238). Multi-click places waypoints along a road;
// the first click anchors the curve to the road under it. A live preview fits a
// clothoid through the points and distributes the selected prop every `spacing`
// metres, projecting each instance onto the anchor road — one ghost handle per
// prop (ghost==commit). Enter or double-click BAKES: the distributed instances
// become individually editable props, added as ONE undo macro (GW-2 step 17).
// Backspace removes the last point, `[`/`]` adjust spacing, Esc cancels. Nothing
// enters the network until the bake. Headless by construction: ToolEvent in,
// commands + PreviewGeometry out.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/id.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include "document/library_manifest.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class PropCurveTool : public Tool {
  Q_OBJECT

public:
  PropCurveTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The prop asset the bake distributes. MainWindow wires this to the merged
  /// Library's default Tree asset. An incompatible/unset item makes the first
  /// click toast rather than start a curve.
  void set_params_provider(std::function<LibraryItem()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Placed points (handles), the ghost polyline to the cursor, and one handle
  /// per distributed prop the bake will author.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] std::size_t point_count() const { return points_.size(); }

  [[nodiscard]] double spacing_m() const { return spacing_m_; }

  [[nodiscard]] QString instruction() const override;

private:
  [[nodiscard]] LibraryItem current_item() const;
  void place_point(double world_x, double world_y);
  void adjust_spacing(double delta);
  void bake();
  void reset_session();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;
  std::vector<Waypoint> points_;
  std::optional<RoadId> anchor_;
  std::optional<Waypoint> cursor_;
  double spacing_m_ = 5.0; ///< distance between props [m]
  /// Seeds the PropSet draw (one per placed instance). Held for the whole
  /// session and re-rolled per session in reset_session, so a mixed set scatters
  /// the SAME models in the preview and the bake (preview == commit); a plain
  /// Tree ignores it.
  std::uint32_t session_seed_ = 0;
};

} // namespace roadmaker::editor
