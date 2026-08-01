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

/// Scenario commands and their stack (p8-s1 PR-D, issue #245). See
/// osc/edit.hpp for the contract; this file holds two disciplines that are
/// easy to lose in a refactor:
///
///   1. VALIDATE IN THE FACTORY, SNAPSHOT AT APPLY. A factory refuses what it
///      can see is wrong and returns a REFUSING COMMAND, never nullptr. The
///      undo snapshot is taken inside `apply`, not in the factory, so a
///      command that is pushed, undone and REDONE snapshots the state it is
///      actually replacing each time rather than a stale one.
///   2. AN UNAPPLIED COMMAND MUST NOT REVERT. Every command below carries an
///      `applied_` flag and reverts to a no-op when it is false, so a stack
///      bug cannot write a default-constructed snapshot into a live document.

#include "roadmaker/osc/edit.hpp"

#include "roadmaker/osc/decompose.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace roadmaker::osc::edit {

// --- the stack ---------------------------------------------------------------

Expected<void> ScenarioStack::push(Scenario& scenario, std::unique_ptr<Command> command) {
  if (command == nullptr) {
    return make_error(ErrorCode::InvalidArgument, "push: null command");
  }
  if (auto applied = command->apply(scenario); !applied.has_value()) {
    return applied;
  }
  // No discard() pass over the truncated tail, unlike edit::EditStack: those
  // commands hold value snapshots and no reserved arena slots, so dropping
  // them frees everything they own.
  commands_.erase(commands_.begin() + static_cast<std::ptrdiff_t>(cursor_), commands_.end());
  commands_.push_back(std::move(command));
  cursor_ = commands_.size();
  enforce_depth_limit();
  return {};
}

Expected<void> ScenarioStack::undo(Scenario& scenario) {
  if (!can_undo()) {
    return make_error(ErrorCode::InvalidArgument, "undo: nothing to undo");
  }
  if (auto reverted = commands_[cursor_ - 1]->revert(scenario); !reverted.has_value()) {
    return reverted;
  }
  --cursor_;
  return {};
}

Expected<void> ScenarioStack::redo(Scenario& scenario) {
  if (!can_redo()) {
    return make_error(ErrorCode::InvalidArgument, "redo: nothing to redo");
  }
  if (auto applied = commands_[cursor_]->apply(scenario); !applied.has_value()) {
    return applied;
  }
  ++cursor_;
  return {};
}

void ScenarioStack::clear() {
  commands_.clear();
  cursor_ = 0;
}

void ScenarioStack::set_depth_limit(std::size_t limit) {
  depth_limit_ = std::max<std::size_t>(limit, 1);
  enforce_depth_limit();
}

void ScenarioStack::enforce_depth_limit() {
  // Drops the OLDEST entries, which are applied: forgetting how to undo them
  // leaves their edits in place, which is what a depth limit means.
  if (commands_.size() <= depth_limit_) {
    return;
  }
  const std::size_t excess = commands_.size() - depth_limit_;
  commands_.erase(commands_.begin(), commands_.begin() + static_cast<std::ptrdiff_t>(excess));
  cursor_ -= std::min(cursor_, excess);
}

namespace {

// --- shared pieces -----------------------------------------------------------

/// A factory's refusal, shaped as a Command so the caller pushes and reads the
/// error like any other failure rather than dereferencing a nullptr.
class RefusingCommand final : public Command {
public:
  RefusingCommand(std::string_view label, Error error) : label_(label), error_(std::move(error)) {}

  Expected<void> apply(Scenario&) override { return tl::unexpected<Error>(error_); }

  Expected<void> revert(Scenario&) override { return tl::unexpected<Error>(error_); }

  [[nodiscard]] std::string_view name() const override { return label_; }

private:
  std::string_view label_;
  Error error_;
};

std::unique_ptr<Command> refuse(std::string_view label, std::string message, std::string context) {
  return std::make_unique<RefusingCommand>(label,
                                           Error{.code = ErrorCode::InvalidArgument,
                                                 .message = std::move(message),
                                                 .context = std::move(context)});
}

[[nodiscard]] bool has_entity(const Scenario& scenario, std::string_view name) {
  return std::any_of(scenario.entities.scenario_objects.begin(),
                     scenario.entities.scenario_objects.end(),
                     [name](const ScenarioObject& object) { return object.name == name; });
}

/// Refuses a road-relative position that names nothing (p8-s2, issue #246).
///
/// The same checks `validate_scenario` makes, brought FORWARD to the factory on
/// purpose: the writer's version fires when the user tries to save, by which
/// point the actor has been on screen for an hour and the message is about a
/// file rather than about the placement that caused it. A `<WorldPosition>` is
/// exempt because it names no road.
///
/// Note this deliberately does NOT check the road or lane against a live
/// `RoadNetwork`: an `osc::edit` command takes a `Scenario&` alone (ADR-0014's
/// amendment), and the ids are OpenDRIVE strings that may legitimately name a
/// network this process has not loaded.
[[nodiscard]] Expected<void> check_position(const Position& position) {
  const std::string* road_id = nullptr;
  const std::string* lane_id = nullptr;
  double station = 0.0;

  if (const auto* road = std::get_if<RoadPosition>(&position)) {
    road_id = &road->road_id;
    station = road->s;
  } else if (const auto* lane = std::get_if<LanePosition>(&position)) {
    road_id = &lane->road_id;
    lane_id = &lane->lane_id;
    station = lane->s;
  } else {
    return {};
  }

  if (road_id->empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a road-relative position names no road",
                      "Storyboard/Init/Actions/Private/PrivateAction/TeleportAction/Position");
  }
  if (lane_id != nullptr && lane_id->empty()) {
    return make_error(ErrorCode::InvalidArgument,
                      "a lane position names no lane",
                      "Storyboard/Init/Actions/Private/PrivateAction/TeleportAction/Position/"
                      "LanePosition/@laneId");
  }
  if (station < 0.0) {
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("s-coordinate {} is negative", station),
                      "Storyboard/Init/Actions/Private/PrivateAction/TeleportAction/Position");
  }
  return {};
}

// --- sync_traffic_signals ----------------------------------------------------

class SyncTrafficSignalsCommand final : public Command {
public:
  SyncTrafficSignalsCommand(std::vector<TrafficSignalController> controllers,
                            std::vector<Diagnostic> findings)
      : controllers_(std::move(controllers)), findings_(std::move(findings)) {}

  Expected<void> apply(Scenario& scenario) override {
    before_ = scenario.road_network.traffic_signal_controllers;

    std::vector<TrafficSignalController> merged = before_;
    for (const TrafficSignalController& fresh : controllers_) {
      const auto existing = std::find_if(
          merged.begin(), merged.end(), [&fresh](const TrafficSignalController& candidate) {
            return candidate.name == fresh.name;
          });
      if (existing == merged.end()) {
        merged.push_back(fresh);
        continue;
      }
      // Overwrite IN PLACE, carrying over what the decomposition does not
      // produce. `delay`/`reference` are authored relationships between
      // controllers and `preserved` is foreign content — regenerating a cycle
      // is not a reason to drop either.
      const bool phases_carried_foreign_content =
          std::any_of(existing->phases.begin(), existing->phases.end(), [](const Phase& phase) {
            return !phase.preserved.attributes.empty() || !phase.preserved.children.empty();
          });
      if (phases_carried_foreign_content && !reported_phase_loss_) {
        reported_phase_loss_ = true;
        findings_.push_back(
            Diagnostic{.severity = Severity::Warning,
                       .location = fmt::format("TrafficSignalController name={}", existing->name),
                       .message = "the replaced cycle's phases carried content this version does "
                                  "not model; it belonged to those phases and does not survive "
                                  "their regeneration.",
                       .rule_id = {},
                       .road = {},
                       .lane = {}});
      }
      TrafficSignalController replacement = fresh;
      replacement.delay = existing->delay;
      replacement.reference = existing->reference;
      replacement.preserved = existing->preserved;
      *existing = std::move(replacement);
    }

    scenario.road_network.traffic_signal_controllers = std::move(merged);
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    scenario.road_network.traffic_signal_controllers = before_;
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Sync Traffic Signals"; }

  [[nodiscard]] std::span<const Diagnostic> findings() const override { return findings_; }

private:
  std::vector<TrafficSignalController> controllers_;
  std::vector<Diagnostic> findings_;
  std::vector<TrafficSignalController> before_;
  bool applied_ = false;
  bool reported_phase_loss_ = false;
};

// --- set_logic_file ----------------------------------------------------------

class SetLogicFileCommand final : public Command {
public:
  explicit SetLogicFileCommand(std::string filepath) : filepath_(std::move(filepath)) {}

  Expected<void> apply(Scenario& scenario) override {
    before_ = scenario.road_network.logic_file;
    FileRef next;
    // Keep the existing element's preserved tier: only @filepath is being set.
    if (before_.has_value()) {
      next.preserved = before_->preserved;
    }
    next.filepath = filepath_;
    scenario.road_network.logic_file = std::move(next);
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    scenario.road_network.logic_file = before_;
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Set Logic File"; }

private:
  std::string filepath_;
  std::optional<FileRef> before_;
  bool applied_ = false;
};

// --- add_scenario_object -----------------------------------------------------

class AddScenarioObjectCommand final : public Command {
public:
  explicit AddScenarioObjectCommand(ScenarioObject object) : object_(std::move(object)) {}

  Expected<void> apply(Scenario& scenario) override {
    // Re-checked at apply, not only in the factory: a redo runs against a
    // document the intervening undos may have changed.
    if (has_entity(scenario, object_.name)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("an entity named '{}' already exists", object_.name),
                        "Entities/ScenarioObject/@name");
    }
    scenario.entities.scenario_objects.push_back(object_);
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    // The command appended, so the entry it owns is the last one — and it is
    // still the last one, because a stack only ever reverts the most recent
    // command.
    if (!scenario.entities.scenario_objects.empty()) {
      scenario.entities.scenario_objects.pop_back();
    }
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Add Actor"; }

private:
  ScenarioObject object_;
  bool applied_ = false;
};

// --- remove_scenario_object --------------------------------------------------

class RemoveScenarioObjectCommand final : public Command {
public:
  explicit RemoveScenarioObjectCommand(std::string name) : target_(std::move(name)) {}

  Expected<void> apply(Scenario& scenario) override {
    std::vector<ScenarioObject>& objects = scenario.entities.scenario_objects;
    const auto found = std::find_if(objects.begin(),
                                    objects.end(),
                                    [this](const ScenarioObject& o) { return o.name == target_; });
    if (found == objects.end()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("no entity named '{}'", target_),
                        "Entities/ScenarioObject/@name");
    }

    index_ = static_cast<std::size_t>(std::distance(objects.begin(), found));
    object_ = *found;
    objects.erase(found);

    // Every <Private> that referenced it goes too. Leaving one behind produces
    // a dangling entityRef, which write_xosc refuses — so a removal that
    // looked like it succeeded would leave the document unwritable.
    privates_before_ = scenario.storyboard.init.actions.privates;
    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    privates.erase(std::remove_if(privates.begin(),
                                  privates.end(),
                                  [this](const Private& p) { return p.entity_ref == target_; }),
                   privates.end());
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    std::vector<ScenarioObject>& objects = scenario.entities.scenario_objects;
    const std::size_t at = std::min(index_, objects.size());
    objects.insert(objects.begin() + static_cast<std::ptrdiff_t>(at), object_);
    scenario.storyboard.init.actions.privates = privates_before_;
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Remove Actor"; }

private:
  std::string target_;
  ScenarioObject object_;
  std::vector<Private> privates_before_;
  std::size_t index_ = 0;
  bool applied_ = false;
};

// --- set_entity_init_position ------------------------------------------------

/// The `RawXml` a position carries, whichever alternative it is. Used to move a
/// preserved tier across an in-place coordinate change, and to notice when one
/// would be lost because the position TYPE changed.
const RawXml& position_preserved(const Position& position) {
  return std::visit([](const auto& pose) -> const RawXml& { return pose.preserved; }, position);
}

void set_position_preserved(Position& position, RawXml preserved) {
  std::visit([&preserved](auto& pose) { pose.preserved = std::move(preserved); }, position);
}

const char* position_element(const Position& position) {
  return std::visit(
      [](const auto& pose) {
        using T = std::decay_t<decltype(pose)>;
        if constexpr (std::is_same_v<T, WorldPosition>) {
          return "WorldPosition";
        } else if constexpr (std::is_same_v<T, RoadPosition>) {
          return "RoadPosition";
        } else {
          return "LanePosition";
        }
      },
      position);
}

class SetEntityInitPositionCommand final : public Command {
public:
  SetEntityInitPositionCommand(std::string entity, Position position)
      : entity_(std::move(entity)), position_(std::move(position)) {}

  Expected<void> apply(Scenario& scenario) override {
    if (!has_entity(scenario, entity_)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("no entity named '{}'", entity_),
                        "Storyboard/Init/Actions/Private/@entityRef");
    }

    // One exact snapshot of the whole subtree; see osc/edit.hpp for why this
    // is deliberately coarser than the mutation.
    before_ = scenario.storyboard.init.actions;
    findings_.clear();

    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    auto owner = std::find_if(privates.begin(), privates.end(), [this](const Private& p) {
      return p.entity_ref == entity_;
    });
    if (owner == privates.end()) {
      Private fresh;
      fresh.entity_ref = entity_;
      privates.push_back(std::move(fresh));
      owner = std::prev(privates.end());
    }

    const auto teleporting =
        std::find_if(owner->actions.begin(), owner->actions.end(), [](const PrivateAction& action) {
          return action.teleport.has_value();
        });
    if (teleporting == owner->actions.end()) {
      PrivateAction action;
      TeleportAction teleport;
      teleport.position = position_;
      action.teleport = std::move(teleport);
      owner->actions.push_back(std::move(action));
    } else {
      Position& current = teleporting->teleport->position;
      Position next = position_;
      if (current.index() == next.index()) {
        // Only the coordinates change: the action's and the position's own
        // preserved tiers belong to the element, not to the value.
        set_position_preserved(next, position_preserved(current));
      } else if (!position_preserved(current).attributes.empty() ||
                 !position_preserved(current).children.empty()) {
        // ★ RETYPING A POSITION DROPS ITS PRESERVED TIER, and says so. A
        // <WorldPosition>'s foreign attributes are meaningless on a
        // <LanePosition> — they name a different element — so carrying them
        // across would emit them on a element that never had them. Dropping
        // them silently is what the never-drop contract (ADR-0014 §6) exists to
        // prevent, so it is reported instead.
        findings_.push_back(
            Diagnostic{.severity = Severity::Warning,
                       .location = fmt::format("Storyboard/Init/Actions/Private[@entityRef='{}']"
                                               "/PrivateAction/TeleportAction/Position",
                                               entity_),
                       .message = fmt::format("the entity's position changed from <{}> to <{}>, so "
                                              "content preserved on the old element was dropped: "
                                              "it does not belong to the new one",
                                              position_element(current),
                                              position_element(next)),
                       .rule_id = {},
                       .road = {},
                       .lane = {}});
      }
      current = std::move(next);
    }

    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    scenario.storyboard.init.actions = before_;
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Place Actor"; }

  [[nodiscard]] std::span<const Diagnostic> findings() const override { return findings_; }

private:
  std::string entity_;
  Position position_;
  InitActions before_;
  std::vector<Diagnostic> findings_;
  bool applied_ = false;
};

// --- set_entity_init_speed ---------------------------------------------------

/// Gives an entity its `<Init>` initial speed.
///
/// APPENDS A SECOND `<PrivateAction>` rather than setting `longitudinal` on the
/// teleport's: the schema's choice is per-element, so a file holds one action
/// per arm, and building the model the READER would have produced from the same
/// file is what keeps the round trip honest (osc/scenario.hpp, PrivateAction).
class SetEntityInitSpeedCommand final : public Command {
public:
  SetEntityInitSpeedCommand(std::string entity, double speed)
      : entity_(std::move(entity)), speed_(speed) {}

  Expected<void> apply(Scenario& scenario) override {
    if (!has_entity(scenario, entity_)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("no entity named '{}'", entity_),
                        "Storyboard/Init/Actions/Private/@entityRef");
    }

    before_ = scenario.storyboard.init.actions;

    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    auto owner = std::find_if(privates.begin(), privates.end(), [this](const Private& p) {
      return p.entity_ref == entity_;
    });
    if (owner == privates.end()) {
      Private fresh;
      fresh.entity_ref = entity_;
      privates.push_back(std::move(fresh));
      owner = std::prev(privates.end());
    }

    const auto existing =
        std::find_if(owner->actions.begin(), owner->actions.end(), [](const PrivateAction& action) {
          return action.longitudinal.has_value() && action.longitudinal->speed.has_value();
        });
    if (existing == owner->actions.end()) {
      SpeedAction speed;
      speed.absolute_target = AbsoluteTargetSpeed{.value = speed_, .preserved = {}};
      LongitudinalAction longitudinal;
      longitudinal.speed = std::move(speed);
      PrivateAction action;
      action.longitudinal = std::move(longitudinal);
      owner->actions.push_back(std::move(action));
    } else if (existing->longitudinal->speed->absolute_target.has_value()) {
      // Only the value changes. The transition dynamics, the target's preserved
      // tier and the action's own belong to the element, not to the number —
      // overwriting a foreign file's `dynamicsShape="linear"` because the user
      // nudged a speed spin box would be an edit nobody asked for.
      existing->longitudinal->speed->absolute_target->value = speed_;
    } else {
      // ★ The entity's speed is set by a target this version does not model — a
      // <RelativeTargetSpeed>, riding `target_preserved`. <SpeedActionTarget>
      // is a 1..1 union, so writing an <AbsoluteTargetSpeed> beside it would
      // emit BOTH and produce a file no parser accepts; silently deleting the
      // relative one is the drop ADR-0014 §6 forbids. Refusing is the only
      // honest third option, and the message says what to do about it.
      //
      // The document is untouched: this returns before any mutation the caller
      // could see, and `before_` is restored to be certain of it.
      scenario.storyboard.init.actions = before_;
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entity '{}' takes its initial speed from a target this "
                                    "version does not model (a <RelativeTargetSpeed>); remove it "
                                    "in the source file before setting an absolute speed",
                                    entity_),
                        "Storyboard/Init/Actions/Private/PrivateAction/LongitudinalAction/"
                        "SpeedAction/SpeedActionTarget");
    }

    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    scenario.storyboard.init.actions = before_;
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Set Actor Speed"; }

private:
  std::string entity_;
  double speed_ = 0.0;
  InitActions before_;
  bool applied_ = false;
};

// --- place_scenario_object ---------------------------------------------------

/// Adds an entity AND places it, as one undoable step.
///
/// ★ ONE COMMAND, NOT TWO PUSHED TOGETHER. Placing an actor is one gesture, so
/// it must be one undo entry; and it has to be one KERNEL command rather than a
/// QUndoCommand with two children, because GW-6's evidence is a headless Python
/// replay and a two-call sequence is not the same thing to replay as one.
///
/// A partial apply is impossible by construction: both halves are validated
/// before either mutates, and the revert unwinds in the reverse order.
class PlaceScenarioObjectCommand final : public Command {
public:
  PlaceScenarioObjectCommand(ScenarioObject object, Position position)
      : object_(std::move(object)), position_(std::move(position)) {}

  Expected<void> apply(Scenario& scenario) override {
    // Re-checked at apply for the AddScenarioObjectCommand reason: a redo runs
    // against a document the intervening undos may have changed.
    if (has_entity(scenario, object_.name)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("an entity named '{}' already exists", object_.name),
                        "Entities/ScenarioObject/@name");
    }

    init_before_ = scenario.storyboard.init.actions;
    scenario.entities.scenario_objects.push_back(object_);

    Private entry;
    entry.entity_ref = object_.name;
    PrivateAction action;
    TeleportAction teleport;
    teleport.position = position_;
    action.teleport = std::move(teleport);
    entry.actions.push_back(std::move(action));
    scenario.storyboard.init.actions.privates.push_back(std::move(entry));

    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    scenario.storyboard.init.actions = init_before_;
    if (!scenario.entities.scenario_objects.empty()) {
      scenario.entities.scenario_objects.pop_back();
    }
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Place Actor"; }

private:
  ScenarioObject object_;
  Position position_;
  InitActions init_before_;
  bool applied_ = false;
};

// --- rename_scenario_object --------------------------------------------------

/// Renames an entity AND every `entityRef` that resolved through it.
///
/// ★ THE REFERENCES ARE THE WHOLE POINT. `@name` is the key `<Private>` resolves
/// through, so renaming the entity alone leaves a dangling `entityRef` that
/// `write_xosc` refuses — a rename that appeared to succeed would make the
/// document unsavable. The `remove_scenario_object` lesson, met a second time.
class RenameScenarioObjectCommand final : public Command {
public:
  RenameScenarioObjectCommand(std::string from, std::string to)
      : from_(std::move(from)), to_(std::move(to)) {}

  Expected<void> apply(Scenario& scenario) override {
    std::vector<ScenarioObject>& objects = scenario.entities.scenario_objects;
    const auto found = std::find_if(objects.begin(),
                                    objects.end(),
                                    [this](const ScenarioObject& o) { return o.name == from_; });
    if (found == objects.end()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("no entity named '{}'", from_),
                        "Entities/ScenarioObject/@name");
    }
    if (has_entity(scenario, to_)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("an entity named '{}' already exists", to_),
                        "Entities/ScenarioObject/@name");
    }

    found->name = to_;
    renamed_refs_.clear();
    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    for (std::size_t index = 0; index < privates.size(); ++index) {
      if (privates[index].entity_ref == from_) {
        privates[index].entity_ref = to_;
        renamed_refs_.push_back(index);
      }
    }

    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    std::vector<ScenarioObject>& objects = scenario.entities.scenario_objects;
    const auto found = std::find_if(
        objects.begin(), objects.end(), [this](const ScenarioObject& o) { return o.name == to_; });
    if (found != objects.end()) {
      found->name = from_;
    }
    // Only the references this command changed, by INDEX: a blanket
    // to_ -> from_ sweep would also rewrite a reference that already said `to_`
    // before the rename, which this command never touched.
    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    for (const std::size_t index : renamed_refs_) {
      if (index < privates.size()) {
        privates[index].entity_ref = from_;
      }
    }
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Rename Actor"; }

private:
  std::string from_;
  std::string to_;
  std::vector<std::size_t> renamed_refs_;
  bool applied_ = false;
};

// --- set_scenario_object_bounding_box ----------------------------------------

class SetBoundingBoxCommand final : public Command {
public:
  SetBoundingBoxCommand(std::string entity, BoundingBox box)
      : entity_(std::move(entity)), box_(std::move(box)) {}

  Expected<void> apply(Scenario& scenario) override {
    BoundingBox* target = find_box(scenario);
    if (target == nullptr) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entity '{}' has no bounding box to set: it is neither a "
                                    "<Vehicle> nor a <Pedestrian>",
                                    entity_),
                        "Entities/ScenarioObject/BoundingBox");
    }
    before_ = *target;
    BoundingBox next = box_;
    // The element's preserved tier belongs to the element, not to the numbers.
    next.preserved = target->preserved;
    *target = std::move(next);
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (!applied_) {
      return {};
    }
    if (BoundingBox* target = find_box(scenario)) {
      *target = before_;
    }
    applied_ = false;
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return "Resize Actor"; }

private:
  BoundingBox* find_box(Scenario& scenario) {
    for (ScenarioObject& object : scenario.entities.scenario_objects) {
      if (object.name != entity_) {
        continue;
      }
      if (auto* vehicle = std::get_if<Vehicle>(&object.entity_object)) {
        return &vehicle->bounding_box;
      }
      if (auto* pedestrian = std::get_if<Pedestrian>(&object.entity_object)) {
        return &pedestrian->bounding_box;
      }
      return nullptr;
    }
    return nullptr;
  }

  std::string entity_;
  BoundingBox box_;
  BoundingBox before_;
  bool applied_ = false;
};

// --- routes (p8-s3, issue #247) ----------------------------------------------

/// Finds `entity`'s `<RoutingAction>` in `<Init>`, or nullptr.
RoutingAction* find_routing(Scenario& scenario, const std::string& entity) {
  for (Private& entry : scenario.storyboard.init.actions.privates) {
    if (entry.entity_ref != entity) {
      continue;
    }
    for (PrivateAction& action : entry.actions) {
      if (action.routing.has_value()) {
        return &*action.routing;
      }
    }
  }
  return nullptr;
}

const Route* find_route(const Scenario& scenario, std::string_view entity) {
  for (const Private& entry : scenario.storyboard.init.actions.privates) {
    if (entry.entity_ref != entity) {
      continue;
    }
    for (const PrivateAction& action : entry.actions) {
      if (action.routing.has_value() && action.routing->assign_route.has_value() &&
          action.routing->assign_route->route.has_value()) {
        return &*action.routing->assign_route->route;
      }
    }
  }
  return nullptr;
}

/// Every route command snapshots the whole `<Init><Actions>` subtree and
/// restores it wholesale.
///
/// Deliberately coarser than each mutation, and for the reason
/// SetEntityInitPositionCommand states: a route edit can create a `<Private>`,
/// create a `<PrivateAction>`, replace a `<Route>` or move one waypoint, and
/// ONE exact value snapshot is one code path where undoing each separately is
/// four. An init block is small, and byte-identity after apply->revert is the
/// property this is measured on — not how little was copied.
class RouteCommand : public Command {
public:
  RouteCommand(std::string_view label, std::string entity)
      : label_(label), entity_(std::move(entity)) {}

  Expected<void> apply(Scenario& scenario) override {
    InitActions before = scenario.storyboard.init.actions;
    if (const Expected<void> mutated = mutate(scenario); !mutated) {
      scenario.storyboard.init.actions = std::move(before); // untouched on failure
      return mutated;
    }
    before_ = std::move(before);
    applied_ = true;
    return {};
  }

  Expected<void> revert(Scenario& scenario) override {
    if (applied_) {
      scenario.storyboard.init.actions = before_;
      applied_ = false;
    }
    return {};
  }

  [[nodiscard]] std::string_view name() const override { return label_; }

protected:
  virtual Expected<void> mutate(Scenario& scenario) = 0;

  [[nodiscard]] const std::string& entity() const { return entity_; }

private:
  std::string_view label_;
  std::string entity_;
  InitActions before_;
  bool applied_ = false;
};

class AssignRouteCommand final : public RouteCommand {
public:
  AssignRouteCommand(std::string entity, Route route)
      : RouteCommand("Assign Route", std::move(entity)), route_(std::move(route)) {}

protected:
  Expected<void> mutate(Scenario& scenario) override {
    if (RoutingAction* existing = find_routing(scenario, entity())) {
      existing->assign_route = AssignRouteAction{.route = route_, .preserved = {}};
      return {};
    }
    std::vector<Private>& privates = scenario.storyboard.init.actions.privates;
    auto owner = std::find_if(privates.begin(), privates.end(), [this](const Private& p) {
      return p.entity_ref == entity();
    });
    if (owner == privates.end()) {
      Private fresh;
      fresh.entity_ref = entity();
      privates.push_back(std::move(fresh));
      owner = std::prev(privates.end());
    }
    PrivateAction action;
    action.routing = RoutingAction{
        .assign_route = AssignRouteAction{.route = route_, .preserved = {}}, .preserved = {}};
    owner->actions.push_back(std::move(action));
    return {};
  }

private:
  Route route_;
};

class ClearRouteCommand final : public RouteCommand {
public:
  explicit ClearRouteCommand(std::string entity) : RouteCommand("Clear Route", std::move(entity)) {}

protected:
  Expected<void> mutate(Scenario& scenario) override {
    for (Private& entry : scenario.storyboard.init.actions.privates) {
      if (entry.entity_ref != entity()) {
        continue;
      }
      // Erase the whole <PrivateAction> when routing was its only arm, and only
      // the arm otherwise — leaving an armless action behind would emit an empty
      // <PrivateAction>, which no schema accepts.
      for (auto action = entry.actions.begin(); action != entry.actions.end(); ++action) {
        if (!action->routing.has_value()) {
          continue;
        }
        if (action->teleport.has_value() || action->longitudinal.has_value()) {
          action->routing.reset();
        } else {
          entry.actions.erase(action);
        }
        return {};
      }
    }
    return make_error(ErrorCode::InvalidArgument,
                      fmt::format("entity '{}' has no route to clear", entity()),
                      "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction");
  }
};

class SetRouteWaypointCommand final : public RouteCommand {
public:
  SetRouteWaypointCommand(std::string entity, std::size_t index, Position position)
      : RouteCommand("Move Route Waypoint", std::move(entity)), index_(index),
        position_(std::move(position)) {}

protected:
  Expected<void> mutate(Scenario& scenario) override {
    RoutingAction* routing = find_routing(scenario, entity());
    if (routing == nullptr || !routing->assign_route.has_value() ||
        !routing->assign_route->route.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entity '{}' has no route", entity()),
                        "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction");
    }
    Route& route = *routing->assign_route->route;
    if (index_ >= route.waypoints.size()) {
      return make_error(
          ErrorCode::InvalidArgument,
          fmt::format("the route has {} waypoint(s); there is none at index {}",
                      route.waypoints.size(),
                      index_),
          "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction/AssignRouteAction/Route");
    }
    // @routeStrategy and the preserved tier belong to the WAYPOINT, not to the
    // position being moved.
    route.waypoints[index_].position = position_;
    return {};
  }

private:
  std::size_t index_;
  Position position_;
};

class InsertRouteWaypointCommand final : public RouteCommand {
public:
  InsertRouteWaypointCommand(std::string entity, std::size_t index, RouteWaypoint waypoint)
      : RouteCommand("Insert Route Waypoint", std::move(entity)), index_(index),
        waypoint_(std::move(waypoint)) {}

protected:
  Expected<void> mutate(Scenario& scenario) override {
    RoutingAction* routing = find_routing(scenario, entity());
    if (routing == nullptr || !routing->assign_route.has_value() ||
        !routing->assign_route->route.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entity '{}' has no route", entity()),
                        "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction");
    }
    Route& route = *routing->assign_route->route;
    if (index_ > route.waypoints.size()) {
      return make_error(
          ErrorCode::InvalidArgument,
          fmt::format("the route has {} waypoint(s); index {} is past the end",
                      route.waypoints.size(),
                      index_),
          "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction/AssignRouteAction/Route");
    }
    route.waypoints.insert(route.waypoints.begin() + static_cast<std::ptrdiff_t>(index_),
                           waypoint_);
    return {};
  }

private:
  std::size_t index_;
  RouteWaypoint waypoint_;
};

class RemoveRouteWaypointCommand final : public RouteCommand {
public:
  RemoveRouteWaypointCommand(std::string entity, std::size_t index)
      : RouteCommand("Remove Route Waypoint", std::move(entity)), index_(index) {}

protected:
  Expected<void> mutate(Scenario& scenario) override {
    RoutingAction* routing = find_routing(scenario, entity());
    if (routing == nullptr || !routing->assign_route.has_value() ||
        !routing->assign_route->route.has_value()) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("entity '{}' has no route", entity()),
                        "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction");
    }
    Route& route = *routing->assign_route->route;
    if (index_ >= route.waypoints.size()) {
      return make_error(
          ErrorCode::InvalidArgument,
          fmt::format("the route has {} waypoint(s); there is none at index {}",
                      route.waypoints.size(),
                      index_),
          "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction/AssignRouteAction/Route");
    }
    if (route.waypoints.size() <= 2) {
      // ★ Refused, not performed. "At least two waypoints are needed to define
      // a route", so this would produce a document write_xosc refuses — a
      // deletion that appeared to succeed and then made the scenario unsavable.
      return make_error(
          ErrorCode::InvalidArgument,
          "a route needs at least two waypoints; remove the whole route instead",
          "Storyboard/Init/Actions/Private/PrivateAction/RoutingAction/AssignRouteAction/Route");
    }
    route.waypoints.erase(route.waypoints.begin() + static_cast<std::ptrdiff_t>(index_));
    return {};
  }

private:
  std::size_t index_;
};

} // namespace

// --- factories ---------------------------------------------------------------

std::unique_ptr<Command> sync_traffic_signals(const Scenario& /*scenario*/,
                                              const RoadNetwork& network,
                                              JunctionId junction) {
  JunctionSignalDecomposition decomposed = decompose_junction_signals(network, junction);
  if (decomposed.controllers.empty()) {
    // NOT an empty sync: a junction with no cycle is a caller mistake, and
    // treating it as "remove everything" would silently delete an authored
    // controller list.
    return refuse("Sync Traffic Signals",
                  "the junction has no signal cycle to export (stale id, span junction, "
                  "unsignalized, or a static template) — signalize it first",
                  "RoadNetwork/TrafficSignals");
  }
  return std::make_unique<SyncTrafficSignalsCommand>(std::move(decomposed.controllers),
                                                     std::move(decomposed.findings));
}

std::unique_ptr<Command> set_logic_file(const Scenario& /*scenario*/, std::string filepath) {
  if (filepath.empty()) {
    return refuse("Set Logic File",
                  "a LogicFile with an empty filepath points nowhere; omit the element instead",
                  "RoadNetwork/LogicFile/@filepath");
  }
  return std::make_unique<SetLogicFileCommand>(std::move(filepath));
}

std::unique_ptr<Command> add_scenario_object(const Scenario& scenario, ScenarioObject object) {
  if (object.name.empty()) {
    return refuse("Add Actor",
                  "an entity needs a name: it is the key every entityRef resolves through",
                  "Entities/ScenarioObject/@name");
  }
  if (has_entity(scenario, object.name)) {
    return refuse("Add Actor",
                  fmt::format("an entity named '{}' already exists", object.name),
                  "Entities/ScenarioObject/@name");
  }
  return std::make_unique<AddScenarioObjectCommand>(std::move(object));
}

std::unique_ptr<Command> remove_scenario_object(const Scenario& scenario, std::string_view name) {
  if (!has_entity(scenario, name)) {
    return refuse(
        "Remove Actor", fmt::format("no entity named '{}'", name), "Entities/ScenarioObject/@name");
  }
  return std::make_unique<RemoveScenarioObjectCommand>(std::string{name});
}

std::unique_ptr<Command> set_entity_init_position(const Scenario& scenario,
                                                  std::string_view entity_name,
                                                  WorldPosition position) {
  return set_entity_init_pose(scenario, entity_name, Position{std::move(position)});
}

std::unique_ptr<Command>
set_entity_init_pose(const Scenario& scenario, std::string_view entity_name, Position position) {
  if (!has_entity(scenario, entity_name)) {
    return refuse("Place Actor",
                  fmt::format("no entity named '{}'", entity_name),
                  "Storyboard/Init/Actions/Private/@entityRef");
  }
  if (const Expected<void> valid = check_position(position); !valid) {
    return refuse("Place Actor", valid.error().message, valid.error().context);
  }
  return std::make_unique<SetEntityInitPositionCommand>(std::string{entity_name},
                                                        std::move(position));
}

std::unique_ptr<Command>
place_scenario_object(const Scenario& scenario, ScenarioObject object, Position position) {
  if (object.name.empty()) {
    return refuse("Place Actor",
                  "an entity needs a name: it is the key every entityRef resolves through",
                  "Entities/ScenarioObject/@name");
  }
  if (has_entity(scenario, object.name)) {
    return refuse("Place Actor",
                  fmt::format("an entity named '{}' already exists", object.name),
                  "Entities/ScenarioObject/@name");
  }
  if (const Expected<void> valid = check_position(position); !valid) {
    return refuse("Place Actor", valid.error().message, valid.error().context);
  }
  return std::make_unique<PlaceScenarioObjectCommand>(std::move(object), std::move(position));
}

std::unique_ptr<Command>
set_entity_init_speed(const Scenario& scenario, std::string_view entity_name, double speed) {
  if (!has_entity(scenario, entity_name)) {
    return refuse("Set Actor Speed",
                  fmt::format("no entity named '{}'", entity_name),
                  "Storyboard/Init/Actions/Private/@entityRef");
  }
  if (speed < 0.0) {
    // Refused, never clamped: a negative initial speed is a data-entry slip,
    // and silently turning it into 0 would hide the slip rather than report it.
    return refuse("Set Actor Speed",
                  fmt::format("an initial speed of {} m/s is negative", speed),
                  "Storyboard/Init/Actions/Private/PrivateAction/LongitudinalAction/SpeedAction");
  }
  return std::make_unique<SetEntityInitSpeedCommand>(std::string{entity_name}, speed);
}

std::unique_ptr<Command>
rename_scenario_object(const Scenario& scenario, std::string_view from, std::string to) {
  if (!has_entity(scenario, from)) {
    return refuse(
        "Rename Actor", fmt::format("no entity named '{}'", from), "Entities/ScenarioObject/@name");
  }
  if (to.empty()) {
    return refuse("Rename Actor",
                  "an entity needs a name: it is the key every entityRef resolves through",
                  "Entities/ScenarioObject/@name");
  }
  if (to != from && has_entity(scenario, to)) {
    return refuse("Rename Actor",
                  fmt::format("an entity named '{}' already exists", to),
                  "Entities/ScenarioObject/@name");
  }
  return std::make_unique<RenameScenarioObjectCommand>(std::string{from}, std::move(to));
}

// --- routes (p8-s3, issue #247) ----------------------------------------------

std::unique_ptr<Command>
assign_route(const Scenario& scenario, std::string_view entity_name, Route route) {
  if (!has_entity(scenario, entity_name)) {
    return refuse("Assign Route",
                  fmt::format("no entity named '{}'", entity_name),
                  "Storyboard/Init/Actions/Private/@entityRef");
  }
  if (route.name.empty()) {
    return refuse("Assign Route",
                  "a route needs a name: the schema requires it and a simulator resolves a route "
                  "by it",
                  "RoutingAction/AssignRouteAction/Route/@name");
  }
  if (route.waypoints.size() < 2) {
    // Brought forward from save time: a tool must not be able to build a
    // document it then cannot write.
    return refuse("Assign Route",
                  fmt::format("a route needs at least two waypoints; this one has {}",
                              route.waypoints.size()),
                  "RoutingAction/AssignRouteAction/Route/Waypoint");
  }
  return std::make_unique<AssignRouteCommand>(std::string{entity_name}, std::move(route));
}

std::unique_ptr<Command> clear_route(const Scenario& scenario, std::string_view entity_name) {
  if (find_route(scenario, entity_name) == nullptr) {
    return refuse("Clear Route",
                  fmt::format("entity '{}' has no route to clear", entity_name),
                  "RoutingAction/AssignRouteAction/Route");
  }
  return std::make_unique<ClearRouteCommand>(std::string{entity_name});
}

std::unique_ptr<Command> set_route_waypoint(const Scenario& scenario,
                                            std::string_view entity_name,
                                            std::size_t index,
                                            Position position) {
  const Route* route = find_route(scenario, entity_name);
  if (route == nullptr) {
    return refuse("Move Route Waypoint",
                  fmt::format("entity '{}' has no route", entity_name),
                  "RoutingAction/AssignRouteAction/Route");
  }
  if (index >= route->waypoints.size()) {
    return refuse("Move Route Waypoint",
                  fmt::format("the route has {} waypoint(s); there is none at index {}",
                              route->waypoints.size(),
                              index),
                  "RoutingAction/AssignRouteAction/Route/Waypoint");
  }
  if (const Expected<void> valid = check_position(position); !valid) {
    return refuse("Move Route Waypoint", valid.error().message, valid.error().context);
  }
  return std::make_unique<SetRouteWaypointCommand>(
      std::string{entity_name}, index, std::move(position));
}

std::unique_ptr<Command> insert_route_waypoint(const Scenario& scenario,
                                               std::string_view entity_name,
                                               std::size_t index,
                                               RouteWaypoint waypoint) {
  const Route* route = find_route(scenario, entity_name);
  if (route == nullptr) {
    return refuse("Insert Route Waypoint",
                  fmt::format("entity '{}' has no route", entity_name),
                  "RoutingAction/AssignRouteAction/Route");
  }
  if (index > route->waypoints.size()) {
    return refuse("Insert Route Waypoint",
                  fmt::format("the route has {} waypoint(s); index {} is past the end",
                              route->waypoints.size(),
                              index),
                  "RoutingAction/AssignRouteAction/Route/Waypoint");
  }
  if (waypoint.route_strategy.empty()) {
    return refuse("Insert Route Waypoint",
                  "a waypoint needs a routeStrategy, which the schema requires",
                  "RoutingAction/AssignRouteAction/Route/Waypoint/@routeStrategy");
  }
  if (const Expected<void> valid = check_position(waypoint.position); !valid) {
    return refuse("Insert Route Waypoint", valid.error().message, valid.error().context);
  }
  return std::make_unique<InsertRouteWaypointCommand>(
      std::string{entity_name}, index, std::move(waypoint));
}

std::unique_ptr<Command>
remove_route_waypoint(const Scenario& scenario, std::string_view entity_name, std::size_t index) {
  const Route* route = find_route(scenario, entity_name);
  if (route == nullptr) {
    return refuse("Remove Route Waypoint",
                  fmt::format("entity '{}' has no route", entity_name),
                  "RoutingAction/AssignRouteAction/Route");
  }
  if (index >= route->waypoints.size()) {
    return refuse("Remove Route Waypoint",
                  fmt::format("the route has {} waypoint(s); there is none at index {}",
                              route->waypoints.size(),
                              index),
                  "RoutingAction/AssignRouteAction/Route/Waypoint");
  }
  if (route->waypoints.size() <= 2) {
    return refuse("Remove Route Waypoint",
                  "a route needs at least two waypoints; remove the whole route instead",
                  "RoutingAction/AssignRouteAction/Route/Waypoint");
  }
  return std::make_unique<RemoveRouteWaypointCommand>(std::string{entity_name}, index);
}

std::unique_ptr<Command> set_scenario_object_bounding_box(const Scenario& scenario,
                                                          std::string_view entity_name,
                                                          BoundingBox box) {
  if (!has_entity(scenario, entity_name)) {
    return refuse("Resize Actor",
                  fmt::format("no entity named '{}'", entity_name),
                  "Entities/ScenarioObject/@name");
  }
  if (box.width <= 0.0 || box.length <= 0.0 || box.height <= 0.0) {
    return refuse("Resize Actor",
                  "an actor's bounding box must have a positive width, length and height",
                  "Entities/ScenarioObject/BoundingBox/Dimensions");
  }
  return std::make_unique<SetBoundingBoxCommand>(std::string{entity_name}, std::move(box));
}

} // namespace roadmaker::osc::edit
