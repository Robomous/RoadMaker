#pragma once

// Lane Carve tool (p2-s6). Press on a lane near a boundary approaching a
// junction, drag toward the junction, release: a turn lane is carved through
// edit::carve_lane — its width ramps 0 -> full over the dragged span and then
// holds full to the road terminus, where junction regeneration absorbs it. ONE
// preview session committing ONE undo entry on release. The insert position and
// side come from the lane boundary nearest the press (nearest_lane_boundary).
// Stays active for repeated carves; Esc cancels an in-flight drag. Headless by
// construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/road.hpp"

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class LaneCarveTool : public Tool {
  Q_OBJECT

public:
  LaneCarveTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

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

  bool pressed_ = false;    ///< a press landed on a lane boundary and the drag is live
  RoadId road0_;            ///< the road the turn lane is being carved on
  int side_ = -1;           ///< +1 left / -1 right of the reference line
  int at_odr_id_ = -1;      ///< the insert position (an existing lane on `side_`)
  double s0_ = 0.0;         ///< the press station
  double lo_ = 0.0;         ///< current taper span low (for the preview highlight)
  double hi_ = 0.0;         ///< current taper span high
  bool span_valid_ = false; ///< the drag has grown past kMinSpan
};

} // namespace roadmaker::editor
