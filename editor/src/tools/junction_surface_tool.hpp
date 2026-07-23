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

// Junction Surface tool (p4-s5, issue #320) — the inspection surface for the
// junction floor's per-connecting-road contributions ("surface spans").
//
// The floor mesher is otherwise a black box: it unions every turn's footprint,
// triangulates, and picks the elevation winner where ribbons overlap by pure
// nearest-distance, with nothing visible to blame an interior artifact on. This
// tool draws each span's footprint and the exact samples the mesher used, and
// exposes the two controls that resolve such an artifact — Include Samples
// (samples-only: the footprint stays in the union, so coverage and the exported
// <boundary> never change) and a free Sort Index where higher wins on overlap.
//
// Inspection only: there is no drag and no preview session. Every key or panel
// action is ONE command, and Document::push_command's junction re-mesh gives
// live re-triangulation in a single undo step.
//
// The active span is tool-local sub-selection, deliberately NOT a SelectionModel
// entry (the CornerTool precedent — there is no SpanId). The owning junction IS
// mirrored into SelectionModel so the Properties pane and the scene tree follow,
// and surface_span_selection_changed() lets the pane bind to the finer state.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/mesh/junction_surface_spans.hpp"
#include "roadmaker/road/id.hpp"

#include <optional>
#include <vector>

#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

/// The tool's sub-selection: one surface span of one junction, named the way the
/// kernel names it — by its connecting road, which regeneration keeps stable.
struct ActiveSurfaceSpan {
  JunctionId junction;
  RoadId road;

  friend bool operator==(const ActiveSurfaceSpan&, const ActiveSurfaceSpan&) = default;
};

class JunctionSurfaceTool : public Tool {
  Q_OBJECT

public:
  JunctionSurfaceTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Every span of the selected junction: its footprint ring as solid lines
  /// (included) or dashed ones (excluded), and its border and centerline
  /// samples as handles. Empty until a junction with a floor is selected.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// The junction whose spans are on show, or an invalid id.
  [[nodiscard]] JunctionId inspected_junction() const { return inspected_; }

  /// The spans of the inspected junction, solved against the CURRENT network.
  [[nodiscard]] std::vector<JunctionSurfaceSpanInfo> spans() const;

  /// The active span, or nullopt. The properties pane binds to this plus
  /// surface_span_selection_changed().
  [[nodiscard]] std::optional<ActiveSurfaceSpan> active_span() const { return active_; }

  /// The active span solved against the current network. nullopt when nothing
  /// is active or its road is no longer a span of the junction.
  [[nodiscard]] std::optional<JunctionSurfaceSpanInfo> active_span_info() const;

  /// Makes `road` the active span of the inspected junction — the panel's row
  /// click. A road that is not a span of it clears the sub-selection.
  void select_span(RoadId road);

  /// Toggles the active span's Include Samples. Reports and pushes nothing when
  /// there is no active span.
  bool toggle_active_included();

  /// Moves the active span's sort index by `delta` (the editor's Raise/Lower —
  /// the kernel deliberately has no such factory).
  bool nudge_active_sort_index(int delta);

signals:
  /// The active span changed, or its authored values did. Carries no payload —
  /// listeners pull active_span()/active_span_info().
  void surface_span_selection_changed();

private:
  /// Re-reads the selection: an arm junction becomes the inspected one, anything
  /// else clears the tool. Keeps the active span when it survives.
  void sync_to_selection();

  /// The span under the cursor: of the footprints containing it, the one with
  /// the highest sort index (ties broken by the later span in connection order,
  /// so the innermost ribbon drawn on top is the one that answers the click).
  [[nodiscard]] std::optional<RoadId> span_under(double world_x, double world_y) const;

  void set_active(std::optional<ActiveSurfaceSpan> span);

  /// Drops every trace of a session (a stale JunctionId across a load is a
  /// crash waiting to happen).
  void reset_all();

  Document& document_;
  SelectionModel& selection_;

  JunctionId inspected_;
  std::optional<ActiveSurfaceSpan> active_;
  std::optional<RoadId> hovered_;
};

} // namespace roadmaker::editor
