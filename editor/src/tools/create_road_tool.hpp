#pragma once

// Create Road tool (issue #13, docs/design/m2/02_editing_tools.md §2).
// Clicks place waypoints (snapped: endpoint > tangent-continuation > grid);
// after two points the ghost polyline gains a live fitted-clothoid preview.
// Enter or double-click commits ONE edit::create_road command, Esc cancels,
// Backspace removes the last point. A first or last point snapped onto a
// road end locks the fit's heading there, so chained roads join G1. No
// preview session: nothing enters the network until commit. Headless by
// construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/edit/snap.hpp"
#include "roadmaker/road/authoring.hpp"

#include <cstddef>
#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;

class CreateRoadTool : public Tool {
  Q_OBJECT

public:
  explicit CreateRoadTool(Document& document, QObject* parent = nullptr);

  /// Cross-section applied on commit; the toolbar template dropdown sets it.
  void set_profile(LaneProfile profile) { profile_ = std::move(profile); }

  [[nodiscard]] const LaneProfile& profile() const { return profile_; }

  void set_snap_options(edit::SnapOptions options) { snap_options_ = options; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Placed waypoints (points), ghost polyline to the cursor, the fitted
  /// clothoid once two points exist, and the snap hint at the cursor.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] std::size_t waypoint_count() const { return points_.size(); }

private:
  /// One placed waypoint plus the continuation heading of the snap that
  /// produced it (set for road-end and tangent snaps, nullopt otherwise).
  struct PlacedPoint {
    Waypoint position;
    std::optional<double> heading;
  };

  [[nodiscard]] std::optional<edit::SnapResult> snap(const Waypoint& cursor) const;

  /// Locked fit headings: the first point's snap heading as-is (pointing
  /// away from the source road = our travel direction), the last point's
  /// reversed (we ARRIVE at that road end).
  [[nodiscard]] EndpointHeadings locked_headings() const;

  void place_point(const ToolEvent& event);
  void commit();
  void reset_session();

  Document& document_;
  LaneProfile profile_ = LaneProfile::two_lane_rural();
  edit::SnapOptions snap_options_{};
  std::vector<PlacedPoint> points_;
  std::optional<edit::SnapResult> hover_snap_;
  std::optional<Waypoint> cursor_;
};

} // namespace roadmaker::editor
