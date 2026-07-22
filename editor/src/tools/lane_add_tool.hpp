// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Lane Add tool (p2-s5). Press on a road body, drag along it, release: a
// self-contained pocket lane (0 -> full -> 0) is carved over the dragged
// station span through edit::add_lane_span — ONE preview session committing ONE
// undo entry on release. The pocket's side comes from the lane the press
// landed on (or the cursor's t sign on a centre pick). Stays active for
// repeated pockets; Esc cancels an in-flight drag. Headless by construction:
// ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/road.hpp"

#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class LaneAddTool : public Tool {
  Q_OBJECT

public:
  LaneAddTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  [[nodiscard]] PreviewGeometry preview() const override;
  [[nodiscard]] QString instruction() const override;

private:
  /// Drops any in-flight drag/preview and returns the tool to rest.
  void reset();

  Document& document_;
  SelectionModel& selection_;

  bool pressed_ = false;    ///< a press landed on a road and the drag is live
  RoadId road0_;            ///< the road the pocket is being drawn on
  int side_ = -1;           ///< +1 left / -1 right of the reference line
  double s0_ = 0.0;         ///< the press station
  double lo_ = 0.0;         ///< current span low (for the preview highlight)
  double hi_ = 0.0;         ///< current span high
  bool span_valid_ = false; ///< the drag has grown past kMinSpan
};

} // namespace roadmaker::editor
