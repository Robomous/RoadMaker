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

// Stop line tool (p4-s3, issue #318). Direct manipulation of a junction arm's
// stop line: hover highlights the solved band, a click makes it active, and
// dragging it along the arm authors the setback. F flips which travel direction
// the band spans.
//
// Every arm HAS a stop line — they are derived, so there is nothing to create
// and the tool only ever edits. The active line is tool-local sub-selection,
// deliberately NOT a SelectionModel entry (there is no StopLineId), following
// the CornerTool precedent; the owning junction IS mirrored into SelectionModel
// so the Properties pane and the scene tree follow, and
// stopline_selection_changed() lets the pane bind to the finer state.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/mesh/junction_stoplines.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"

#include <optional>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// The tool's sub-selection: one stop line, named the way the kernel names it —
/// the junction-facing road end it belongs to.
struct ActiveStopLine {
  JunctionId junction;
  RoadEnd arm;

  friend bool operator==(const ActiveStopLine&, const ActiveStopLine&) = default;
};

class StopLineTool : public Tool {
  Q_OBJECT

public:
  StopLineTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Capture radius [m] around the band's centreline.
  void set_pick_radius(double radius) { pick_radius_ = radius; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The hovered and active bands (line_positions) plus, once a line is active,
  /// a handle at its midpoint.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The active stop line, or nullopt. The properties pane binds to this plus
  /// stopline_selection_changed().
  [[nodiscard]] std::optional<ActiveStopLine> active_stopline() const { return active_; }

  /// The active line solved against the CURRENT network (effective distance and
  /// its bound, the authored flags, the endpoints). nullopt when nothing is
  /// active or the arm no longer carries a line.
  [[nodiscard]] std::optional<JunctionStopLineInfo> active_stopline_info() const;

  [[nodiscard]] bool dragging() const { return press_.has_value(); }

signals:
  /// The active stop line changed (selected, re-selected, or cleared). Carries
  /// no payload — listeners pull active_stopline()/active_stopline_info().
  void stopline_selection_changed();

private:
  /// Solves `line` against the current network. nullopt when the junction or
  /// the arm is gone.
  [[nodiscard]] std::optional<JunctionStopLineInfo> solve(const ActiveStopLine& line) const;

  /// The stop line under the cursor: the nearest band centreline within
  /// pick_radius_, across every junction.
  [[nodiscard]] std::optional<ActiveStopLine> resolve_stopline(const ToolEvent& event) const;

  /// The setback the cursor asks for, from the press-time snapshot: the cursor
  /// projected to the nearest station on the arm road, converted to a distance
  /// from the junction mouth and clamped to what the road leaves room for.
  [[nodiscard]] std::optional<double> distance_for(const ActiveStopLine& line,
                                                   const JunctionStopLineInfo& info,
                                                   double world_x,
                                                   double world_y) const;

  /// Runs one drag frame: begins the preview session on the first move and
  /// replaces it on every later one.
  void update_drag(double world_x, double world_y);

  void set_active(std::optional<ActiveStopLine> line);

  /// Drops every trace of a session (a stale JunctionId across a load is a
  /// crash waiting to happen).
  void reset_all();

  Document& document_;
  SelectionModel& selection_;
  double pick_radius_ = 2.0;

  std::optional<ActiveStopLine> active_;
  std::optional<ActiveStopLine> hovered_;
  /// The press-time snapshot. Every drag frame is computed from it, never from
  /// the live (previewed) network, because update_preview's factory runs
  /// against the BASE state.
  std::optional<JunctionStopLineInfo> press_;
};

} // namespace roadmaker::editor
