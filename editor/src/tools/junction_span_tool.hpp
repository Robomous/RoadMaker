// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Junction Span tool (p4-s4, issue #319) — authoring a VIRTUAL junction, ASAM
// OpenDRIVE 1.9.0 §12.7 (identical in 1.8.1 §12.7): a stretch [sStart, sEnd] of
// an UNINTERRUPTED road that belongs to a junction without cutting it (the
// mid-road crosswalk, the parallel-carriageway crossing).
//
// Interaction: press-drag-release on a road stages that road's [s0, s1] span;
// an optional second drag on ANOTHER road stages the parallel span (the kernel
// caps a span junction at two roads). Enter commits ONE
// edit::create_span_junction — a single undo entry — and selects the junction
// it created. Esc resets and leaves the network byte-identical.
//
// Nothing enters the network until Enter, so — like the Create Junction tool —
// there is NO preview session: the drag lives entirely in PreviewGeometry.
// The stage-and-confirm shape is Create Junction's; the press-snapshot drag
// discipline is the Stop Line tool's.
//
// Headless by construction: ToolEvent in, command + PreviewGeometry out.

#include "roadmaker/road/id.hpp"
#include "roadmaker/road/junction.hpp"

#include <cstddef>
#include <optional>
#include <vector>

#include "document/prop_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class JunctionSpanTool : public Tool {
  Q_OBJECT

public:
  JunctionSpanTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// Lateral reach [m] within which a press grabs a road's reference line.
  void set_snap_threshold(double threshold) { snap_threshold_ = threshold; }

  void activate() override;
  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Every staged span plus the in-flight drag, drawn as a band along the
  /// road's reference line with a handle at each station.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

  /// Spans staged so far (0, 1 or 2) — the tool's confirmable state.
  [[nodiscard]] std::size_t staged_count() const { return spans_.size(); }

  [[nodiscard]] const std::vector<SpanArm>& staged_spans() const { return spans_; }

  [[nodiscard]] bool dragging() const { return drag_road_.has_value(); }

private:
  /// The road the cursor is over: the viewport's own PICK when it named one
  /// (inside a junction the connecting roads overlap the through roads, and
  /// only the pick tells them apart), else the road whose reference line
  /// passes nearest within the snap reach. nullopt when nothing is in reach.
  [[nodiscard]] std::optional<RoadStation> road_under(const ToolEvent& event) const;

  /// Stations the drag along its own road, clamped to [0, length].
  [[nodiscard]] std::optional<double> station_on(RoadId road, double world_x, double world_y) const;

  /// Appends (or replaces the same road's) staged span. Refusals are reported,
  /// never silent.
  void stage(RoadId road, double s0, double s1);

  void commit();
  void reset_session();

  Document& document_;
  SelectionModel& selection_;
  double snap_threshold_ = 12.0;

  std::vector<SpanArm> spans_;

  /// The in-flight drag: the road it started on and both stations. Held apart
  /// from `spans_` so a release that turns out degenerate stages nothing.
  std::optional<RoadId> drag_road_;
  double drag_s0_ = 0.0;
  double drag_s1_ = 0.0;

  /// The connecting road last reported as a refused hover — so the cue is
  /// emitted on entry, not once per mouse-move frame.
  RoadId refused_hover_;
};

} // namespace roadmaker::editor
