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
#include <utility>

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

class SetEntityInitPositionCommand final : public Command {
public:
  SetEntityInitPositionCommand(std::string entity, WorldPosition position)
      : entity_(std::move(entity)), position_(position) {}

  Expected<void> apply(Scenario& scenario) override {
    if (!has_entity(scenario, entity_)) {
      return make_error(ErrorCode::InvalidArgument,
                        fmt::format("no entity named '{}'", entity_),
                        "Storyboard/Init/Actions/Private/@entityRef");
    }

    // One exact snapshot of the whole subtree; see osc/edit.hpp for why this
    // is deliberately coarser than the mutation.
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
      // Only the coordinates change: the action's and the position's own
      // preserved tiers belong to the element, not to the value.
      WorldPosition next = position_;
      next.preserved = teleporting->teleport->position.preserved;
      teleporting->teleport->position = std::move(next);
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

private:
  std::string entity_;
  WorldPosition position_;
  InitActions before_;
  bool applied_ = false;
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
  if (!has_entity(scenario, entity_name)) {
    return refuse("Place Actor",
                  fmt::format("no entity named '{}'", entity_name),
                  "Storyboard/Init/Actions/Private/@entityRef");
  }
  return std::make_unique<SetEntityInitPositionCommand>(std::string{entity_name}, position);
}

} // namespace roadmaker::osc::edit
