// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

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

  /// The single currently-selected road (nullopt when zero or several are
  /// selected). Wired from the SelectionModel; a first click snapped onto this
  /// road's END arms an extend-from-endpoint gesture instead of a new road.
  void set_selected_road(std::optional<RoadId> road) { selected_road_ = road; }

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

  /// Places the first waypoint at a world point — a library road-template drop
  /// arms the tool here so the user continues from the drop location.
  void begin_at(double world_x, double world_y);

  [[nodiscard]] QString instruction() const override;

private:
  /// One placed waypoint plus the continuation heading of the snap that
  /// produced it (set for road-end and tangent snaps, nullopt otherwise).
  /// `kind` records WHICH snap produced the point so the commit path can tell a
  /// genuine chaining intent (RoadEndpoint) from an incidental tangent-ray hit
  /// (TangentContinuation) — only the former locks a fit heading (#352).
  struct PlacedPoint {
    Waypoint position;
    std::optional<double> heading;
    edit::SnapKind kind = edit::SnapKind::Grid; ///< snap that produced this point
    std::optional<RoadId> snap_road;            ///< source road when snapped to its end
    std::optional<edit::SideSnap> side_snap;    ///< target road body when snapped to a side
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
  std::optional<RoadId> selected_road_;
  /// Set when the first placed point anchored on the selected road's END: the
  /// session extends that road to the last point instead of authoring a new one.
  std::optional<RoadEnd> extend_end_;
};

} // namespace roadmaker::editor
