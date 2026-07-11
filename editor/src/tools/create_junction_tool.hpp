#pragma once

// Create Junction tool (issue #17, docs/design/m2/02_editing_tools.md §6).
// Clicks select road ENDS (snapped to the nearest endpoint within proximity);
// each selected end highlights and the status bar counts them. Enter generates
// ONE edit::create_junction command — connecting roads for every permitted
// turn, plus lane links and a flat placeholder surface — Esc clears the
// selection. Nothing enters the network until Enter, so there is no preview
// session. Headless by construction: ToolEvent in, command + PreviewGeometry
// out.

#include "roadmaker/edit/operations.hpp"
#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/road.hpp"

#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

class CreateJunctionTool : public Tool {
  Q_OBJECT

public:
  explicit CreateJunctionTool(Document& document, QObject* parent = nullptr);

  void set_snap_options(edit::SnapOptions options) { snap_options_ = options; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Markers at every selected road end plus the hovered snap candidate.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] std::size_t selected_count() const { return ends_.size(); }

private:
  /// The road end nearest `cursor` within the snap radius (endpoints only),
  /// resolved to a RoadEnd (start/end by proximity).
  [[nodiscard]] std::optional<RoadEnd> snap_end(const Waypoint& cursor) const;

  void toggle(const RoadEnd& end);
  void generate();
  void reset_session();
  void emit_count_status();

  Document& document_;
  edit::SnapOptions snap_options_{};
  std::vector<RoadEnd> ends_;
  std::optional<Waypoint> hover_;
};

} // namespace roadmaker::editor
