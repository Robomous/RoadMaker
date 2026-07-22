// Copyright 2026 Robomous
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Crosswalk & Stop Line tool (p3-s3, issue #222). Click a junction approach: a
// crosswalk carrying the selected asset's parameters places along the arm's
// lane cross-section, together with its stop line, as ONE undo entry. A chevron
// affordance points into the junction while hovering an approach (GW-2 step 10,
// GW-5 step 5). Click-to-act like Create Junction (no rubber-band); the arm is
// resolved from the cursor through nearest_junction_arm, so the interactive
// tool and the Library drag-drop path funnel through identical placement logic.
// Headless by construction: ToolEvent in, commands + PreviewGeometry out.

#include "roadmaker/edit/markings.hpp"

#include <functional>
#include <optional>

#include "document/crosswalk_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class CrosswalkStopLineTool : public Tool {
  Q_OBJECT

public:
  CrosswalkStopLineTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The crosswalk asset the next click places. MainWindow wires this to the
  /// merged Library's default crosswalk asset (mirrors the context-menu
  /// generator's ContextMenuDeps::default_crosswalk_params). Unset falls back to
  /// edit::CrosswalkParams{}.
  void set_params_provider(std::function<edit::CrosswalkParams()> provider);

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;

  /// The chevron pointing into the junction at the hovered approach, else empty.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

private:
  void reset();

  Document& document_;
  SelectionModel& selection_;
  std::function<edit::CrosswalkParams()> params_provider_;
  std::optional<ArmHit> hover_;
};

} // namespace roadmaker::editor
