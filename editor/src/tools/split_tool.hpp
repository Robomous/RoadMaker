// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Split tool (M3a topology UX). A one-shot: hover a road body to see the cut
// marker at the nearest station, click to split the road there
// (edit::split_road, one undo step), then return to Select via request_tool.
// Post-split it selects both halves and names the new ids. Esc also returns to
// Select. Headless by construction: ToolEvent in, commands + PreviewGeometry
// out.

#include "roadmaker/road/road.hpp"

#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class SplitTool : public Tool {
  Q_OBJECT

public:
  SplitTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// A cross marker at the hovered cut point (lines).
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

private:
  /// The road + station the cursor currently points at (updated on hover).
  struct CutHover {
    RoadId road;
    double s = 0.0;
    Waypoint position;
  };

  [[nodiscard]] std::optional<CutHover> hover_at(const ToolEvent& event) const;

  Document& document_;
  SelectionModel& selection_;
  std::optional<CutHover> hover_;
};

} // namespace roadmaker::editor
