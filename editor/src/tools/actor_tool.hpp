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

// The Actor tool (p8-s2, issue #246): click a lane to place a scenario actor,
// drag a placed one to re-anchor it. Headless by construction — ToolEvent in,
// commands + PreviewGeometry out — so its whole interaction runs under gtest.
//
// EVERY MUTATION GOES THROUGH Document::push_scenario_command, so an actor
// placed by this tool lands on the SAME undo stack as the roads beneath it.
// The tool never touches the scenario directly.

#include "roadmaker/osc/catalog.hpp"

#include <optional>
#include <string>

#include "document/actor_placement.hpp"
#include "tools/tool.hpp"

namespace roadmaker::editor {

class Document;
class SelectionModel;

class ActorTool : public Tool {
  Q_OBJECT

public:
  ActorTool(Document& document, SelectionModel& selection, QObject* parent = nullptr);

  /// The archetype the next click places. Defaults to a car; MainWindow wires
  /// this to the Scenario toolbar's kind picker.
  void set_kind(osc::ActorKind kind);

  [[nodiscard]] osc::ActorKind kind() const { return kind_; }

  void deactivate() override;

  [[nodiscard]] bool mouse_press(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_move(const ToolEvent& event) override;
  [[nodiscard]] bool mouse_release(const ToolEvent& event) override;
  [[nodiscard]] bool key_press(int key, Qt::KeyboardModifiers modifiers) override;

  /// The ghost at the hovered (or dragged) anchor, else empty.
  [[nodiscard]] PreviewGeometry preview() const override;

  [[nodiscard]] QString instruction() const override;

private:
  /// LMB held: either a click that will place a new actor, or a drag of the
  /// actor already under the cursor.
  struct PressState {
    double world_x = 0.0;
    double world_y = 0.0;
    /// Non-empty when the press landed on an existing actor — the drag case.
    std::string actor;
    bool dragging = false;
  };

  /// Re-anchors `actor` to `anchor` as ONE undo entry. Used on drag release,
  /// never per motion step: a drag is a preview session plus one command
  /// (docs/design/m2/01_editing_framework.md §3), and a scenario command per
  /// mouse-move would fill the undo stack with the path of the cursor.
  void commit_move(const std::string& actor, const LaneAnchor& anchor);

  /// Where `name` currently stands, or nullopt when it has been declared but
  /// never placed (no `<Private>`), or its position is not a lane position.
  [[nodiscard]] std::optional<ActorPose> actor_pose_of(const std::string& name) const;

  Document& document_;
  SelectionModel& selection_;
  osc::ActorKind kind_ = osc::ActorKind::Car;

  /// The anchor under the cursor, refreshed on every move. Drives the ghost.
  std::optional<LaneAnchor> hovered_;

  /// Why the cursor cannot take an actor, when it cannot. Shown as the
  /// instruction so the refusal is visible BEFORE the click, not after it.
  std::string hover_hint_;

  std::optional<PressState> press_;
};

} // namespace roadmaker::editor
