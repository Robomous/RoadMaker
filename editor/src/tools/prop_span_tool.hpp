// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Prop Span tool (p6-s5, issue #239). Two clicks on one road define a repeating
// run of the selected prop: the first click anchors the span and fixes its
// lateral offset, the second sets the far station. A live preview expands the
// `<repeat>` exactly as the kernel mesher does, one ghost ring per instance.
// Enter or double-click commits ONE `<object>` carrying ONE `<repeat>` — a
// single undo entry (a span is one object, so no macro). `[`/`]` adjust the
// instance spacing, Backspace steps back a click, Esc cancels.
//
// KNOWN LIMITATION (documented for the user in objects-signals.md): a committed
// span is placed as an object + repeat, and move_object relocates the object's
// own s, not the repeat's s — so a baked span can't be dragged to a new
// position this sprint. Repositioning a span = delete and recreate; a follow-up
// issue tracks a span editor over update_objects.
//
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/road/authoring.hpp"
#include "roadmaker/road/id.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "document/library_manifest.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class PropSpanTool : public Tool {
  Q_OBJECT

public:
  PropSpanTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The prop asset the span repeats. MainWindow wires this to the merged
  /// Library's default prop. An incompatible/unset item makes the first click
  /// toast rather than start a span.
  void set_params_provider(std::function<LibraryItem()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_double_click(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// Handles for the anchored stations plus one ghost per repeat instance the
  /// commit will place (ghost == mesh).
  [[nodiscard]] PreviewGeometry preview() const override;

  /// Placed stations so far: 0 (nothing), 1 (anchored), or 2 (span defined).
  [[nodiscard]] std::size_t station_count() const;

  [[nodiscard]] double distance_m() const { return distance_m_; }

  [[nodiscard]] QString instruction() const override;

private:
  [[nodiscard]] LibraryItem current_item() const;
  void adjust_distance(double delta);
  void commit();
  void reset_session();

  Document& document_;
  SelectionModel& selection_;
  std::function<LibraryItem()> params_provider_;

  std::optional<RoadId> anchor_;   ///< the road both stations live on
  double s1_ = 0.0;                ///< first (anchor) station [m]
  double t_ = 0.0;                 ///< lateral offset both stations share [m]
  std::optional<double> s2_;       ///< second station, set by the 2nd click
  std::optional<double> cursor_s_; ///< hovered station on the anchor, for the preview
  LibraryItem resolved_item_;      ///< a PropSet drawn once per session (one model per span)
  double distance_m_ = 5.0;        ///< spacing between instances [m]
  std::uint32_t session_seed_ = 0; ///< seeds the once-per-session PropSet draw
};

} // namespace roadmaker::editor
