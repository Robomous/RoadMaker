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

#include "tools/actor_tool.hpp"

#include "roadmaker/osc/edit.hpp"

#include <QString>
#include <cmath>
#include <string>
#include <utility>

#include "document/document.hpp"
#include "document/selection_model.hpp"

namespace roadmaker::editor {
namespace {

/// A drag has to move this far before it stops being a click [m]. Without it,
/// the hand tremor in a click would re-anchor the actor to where it already is
/// and push a no-op command onto the undo stack.
constexpr double kDragThreshold = 0.25;

} // namespace

ActorTool::ActorTool(Document& document, SelectionModel& selection, QObject* parent)
    : Tool(parent), document_(document), selection_(selection) {}

void ActorTool::set_kind(osc::ActorKind kind) {
  kind_ = kind;
}

void ActorTool::deactivate() {
  press_.reset();
  hovered_.reset();
  hover_hint_.clear();
  emit preview_changed();
}

bool ActorTool::mouse_press(const ToolEvent& event) {
  if ((event.buttons & Qt::LeftButton) == 0) {
    return false;
  }

  PressState state{.world_x = event.world_x, .world_y = event.world_y};

  // An actor already under the cursor is grabbed rather than stacked on. The
  // nearest one wins, and only within kActorGrabRadius — otherwise a click on
  // the far side of the same lane would move an actor instead of placing one.
  //
  // Shares `actor_at` with the Select tool: two hit tests would eventually
  // disagree about what is under the cursor, and the disagreement would only
  // ever show up as "clicking the actor did something else".
  if (const std::optional<std::string> hit =
          actor_at(document_.scenario(), document_.network(), event.world_x, event.world_y)) {
    state.actor = *hit;
  }

  press_ = state;
  return true;
}

bool ActorTool::mouse_move(const ToolEvent& event) {
  hovered_ = nearest_lane_anchor(document_.network(), event.world_x, event.world_y);
  hover_hint_ = hovered_.has_value()
                    ? std::string{}
                    : actor_drop_hint(document_.network(), event.world_x, event.world_y);

  if (press_.has_value() && !press_->actor.empty()) {
    const double moved =
        std::hypot(event.world_x - press_->world_x, event.world_y - press_->world_y);
    if (moved > kDragThreshold) {
      press_->dragging = true;
    }
  }

  emit preview_changed();
  return true;
}

bool ActorTool::mouse_release(const ToolEvent& event) {
  if (!press_.has_value()) {
    return false;
  }
  const PressState state = *press_;
  press_.reset();

  const std::optional<LaneAnchor> anchor =
      nearest_lane_anchor(document_.network(), event.world_x, event.world_y);
  if (!anchor.has_value()) {
    // ★ REFUSED WITH A HINT, never placed in space. An actor with no lane is an
    // actor a simulator has to reject later, and refusing at the click is the
    // point (GW-6 step 2).
    emit toast_requested(
        QString::fromStdString(actor_drop_hint(document_.network(), event.world_x, event.world_y)),
        ToastSeverity::Warning);
    emit preview_changed();
    return true;
  }

  if (state.dragging && !state.actor.empty()) {
    commit_move(state.actor, *anchor);
    emit preview_changed();
    return true;
  }
  if (!state.actor.empty()) {
    // A press on an actor that never became a drag is a SELECT, not a
    // placement — clicking an actor to select it must not stack a second one
    // on top of it.
    selection_.select(SelectionEntry{.actor = state.actor});
    emit preview_changed();
    return true;
  }

  const osc::ActorArchetype& archetype = osc::actor_archetype(kind_);
  const std::string name = next_actor_name(document_.scenario(), archetype.key);

  // ONE command: the entity and its placement, so one Ctrl+Z undoes the whole
  // gesture (osc::edit::place_scenario_object).
  const auto placed = document_.push_scenario_command(osc::edit::place_scenario_object(
      document_.scenario(), osc::make_actor(kind_, name), to_lane_position(*anchor)));
  if (!placed.has_value()) {
    emit toast_requested(QString::fromStdString(placed.error().message), ToastSeverity::Error);
    emit preview_changed();
    return true;
  }

  selection_.select(SelectionEntry{.actor = name});
  emit preview_changed();
  return true;
}

bool ActorTool::key_press(int key, Qt::KeyboardModifiers /*modifiers*/) {
  if (key == Qt::Key_Escape) {
    press_.reset();
    emit preview_changed();
    return true;
  }
  return false;
}

void ActorTool::commit_move(const std::string& actor, const LaneAnchor& anchor) {
  const auto moved = document_.push_scenario_command(osc::edit::set_entity_init_pose(
      document_.scenario(), actor, osc::Position{to_lane_position(anchor)}));
  if (!moved.has_value()) {
    emit toast_requested(QString::fromStdString(moved.error().message), ToastSeverity::Error);
  }
}

PreviewGeometry ActorTool::preview() const {
  PreviewGeometry geometry;
  if (!hovered_.has_value()) {
    return geometry;
  }
  const std::optional<ActorPose> pose = actor_world_pose(document_.network(), *hovered_);
  if (!pose.has_value()) {
    return geometry;
  }
  // ★ The ghost goes through the SAME projection the committed actor does, so
  // what the user sees under the cursor is where the actor lands.
  geometry.add_handle(pose->position[0], pose->position[1], pose->position[2]);
  return geometry;
}

QString ActorTool::instruction() const {
  if (!hover_hint_.empty()) {
    // The refusal is visible BEFORE the click rather than as a toast after it.
    return QString::fromStdString(hover_hint_);
  }
  const osc::ActorArchetype& archetype = osc::actor_archetype(kind_);
  return tr("Click a driving lane to place a %1; drag a placed actor to move it.")
      .arg(QString::fromUtf8(archetype.label.data(),
                             static_cast<qsizetype>(archetype.label.size())));
}

} // namespace roadmaker::editor
