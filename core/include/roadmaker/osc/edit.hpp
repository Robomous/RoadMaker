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

/// Undoable scenario mutation (p8-s1 PR-D, issue #245) — the `.xosc` twin of
/// `edit/command.hpp` + `edit/edit_stack.hpp`.
///
/// WHY THIS IS A SECOND COMMAND LAYER AND NOT AN ENTRY IN THE FIRST.
/// ADR-0014 §1 requires scenario mutation to be kernel-side and stack-driven,
/// so that a GW-6 replay can drive it headlessly from Python — `python/
/// CMakeLists.txt:36` links `roadmaker::core` alone, so anything that lives in
/// the editor cannot be replayed at all. The record's §1 says those mutations
/// are factories "in the existing `roadmaker::edit` namespace", which was
/// written before the model existed and is not implementable as stated:
/// `edit::Command::apply` takes a `RoadNetwork&` (`edit/command.hpp:122`), and
/// a `Scenario` is not arena content — it is a SECOND Layer-0 document in its
/// own file (ADR-0014 §9). See that record's dated amendment.
///
/// So this layer mirrors the first in shape and in contract, over `Scenario&`:
///
///   * apply -> revert leaves `write_xosc(scenario)` BYTE-IDENTICAL to the
///     pre-apply output. This is the `write_xodr` invariant transplanted, and
///     it is what lets a scenario take part in the same undo x10 / redo x10
///     fingerprinting both golden-workflow replays already use.
///   * A failed `apply` leaves the document untouched: commands validate
///     first and mutate after.
///   * Commands capture VALUE SNAPSHOTS of what they replace, never
///     references or iterators into the document they are about to mutate.
///   * A factory given invalid input returns a Command whose `apply` reports
///     the error — NEVER `nullptr`, so a caller can push it and read the
///     refusal like any other failure (the p6-s9 lesson).
///
/// TWO MEMBERS OF `edit::Command` ARE DELIBERATELY ABSENT, and their absence
/// is not an oversight:
///   * no `DirtySet` — nothing is meshed from a scenario, so there is no
///     incremental re-mesh for one to drive;
///   * no `discard()` — there are no reserved arena slots to release, because
///     a scenario holds no arena and no generational ids at all.
/// `findings()` replaces them, for the same reason `edit::Command` grew
/// `follow_records()`: a mutation that had to cope with something must not
/// leave that unsaid.

#pragma once

#include "roadmaker/error.hpp"
#include "roadmaker/export.hpp"
#include "roadmaker/osc/scenario.hpp"
#include "roadmaker/road/id.hpp"
#include "roadmaker/xodr/diagnostic.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace roadmaker {
class RoadNetwork;
} // namespace roadmaker

namespace roadmaker::osc::edit {

/// One undoable mutation of a `Scenario`. See this file's header for the
/// contract every implementation is held to.
class RM_API Command {
public:
  Command() = default;
  Command(const Command&) = delete;
  Command& operator=(const Command&) = delete;
  Command(Command&&) = delete;
  Command& operator=(Command&&) = delete;
  virtual ~Command() = default;

  [[nodiscard]] virtual Expected<void> apply(Scenario& scenario) = 0;
  [[nodiscard]] virtual Expected<void> revert(Scenario& scenario) = 0;

  /// Human-readable operation name for undo menu text ("Sync Traffic Signals").
  [[nodiscard]] virtual std::string_view name() const = 0;

  /// What this command had to cope with — a signal handle that no longer
  /// resolved, a member controller with no live `<controller>`, a replaced
  /// cycle whose phases carried foreign content. Valid after a successful
  /// `apply()`, and KEPT across a revert, since they describe what applying
  /// the command does and a redo does it again (the `follow_records()`
  /// lifetime, `edit/command.hpp:132-137`).
  ///
  /// Default empty: most of these commands cannot surprise anyone. Nothing
  /// here is an error — a command that could not proceed fails instead.
  [[nodiscard]] virtual std::span<const Diagnostic> findings() const { return {}; }
};

/// Headless undo/redo over `Command` — the `.xosc` twin of `edit::EditStack`,
/// and it exists for the same narrow reason: Python and headless parity ONLY.
/// The editor's single stack is Qt's `QUndoStack` and a document must never be
/// driven by both (docs/m2/01_editing_framework.md §1.2).
// RM_API per-method, not on the class: a class-level export would demand a DLL
// interface for the std::vector member on MSVC (C4251). EditStack's rule.
class ScenarioStack {
public:
  ScenarioStack() = default;
  // Not copyable (owns the recorded commands); movable. The explicit deletion
  // also keeps binding generators from instantiating the ill-formed
  // vector<unique_ptr> copy.
  ScenarioStack(const ScenarioStack&) = delete;
  ScenarioStack& operator=(const ScenarioStack&) = delete;
  ScenarioStack(ScenarioStack&&) = default;
  ScenarioStack& operator=(ScenarioStack&&) = default;
  ~ScenarioStack() = default;

  /// Applies the command and records it on success; a failed apply returns the
  /// command's error and records nothing (the document is unchanged per the
  /// Command contract). Pushing truncates the redo tail.
  [[nodiscard]] RM_API Expected<void> push(Scenario& scenario, std::unique_ptr<Command> command);

  [[nodiscard]] RM_API Expected<void> undo(Scenario& scenario);
  [[nodiscard]] RM_API Expected<void> redo(Scenario& scenario);

  [[nodiscard]] bool can_undo() const { return cursor_ > 0; }

  [[nodiscard]] bool can_redo() const { return cursor_ < commands_.size(); }

  /// Recorded commands (applied + redoable).
  [[nodiscard]] std::size_t size() const { return commands_.size(); }

  /// What the most recently applied command had to cope with, or empty when
  /// nothing is applied. `push` takes ownership of the command, so this is the
  /// only way a headless caller can read those findings — and reading them is
  /// the point.
  [[nodiscard]] std::span<const Diagnostic> last_findings() const {
    return cursor_ == 0 ? std::span<const Diagnostic>{} : commands_[cursor_ - 1]->findings();
  }

  RM_API void clear();

  /// Caps recorded history, dropping oldest entries first (their edits stay
  /// applied — they just become un-undoable). Clamped to at least 1.
  RM_API void set_depth_limit(std::size_t limit);

  [[nodiscard]] std::size_t depth_limit() const { return depth_limit_; }

private:
  void enforce_depth_limit();

  std::vector<std::unique_ptr<Command>> commands_;
  std::size_t cursor_ = 0; // commands_[0, cursor_) are currently applied
  std::size_t depth_limit_ = 256;
};

// --- factories ---------------------------------------------------------------
//
// Each takes the CURRENT `Scenario` so it can validate and snapshot at factory
// time, exactly as `edit/operations.hpp`'s factories take the current
// `RoadNetwork`. Create a command and push it immediately.

/// Writes `junction`'s signal timeline into the scenario as one
/// `TrafficSignalController` per member `<controller>`
/// (`osc/decompose.hpp`, ADR-0014 §8).
///
/// MERGES BY `@name` RATHER THAN REPLACING THE LIST. A scenario may reference
/// several junctions, and a wholesale replace would make syncing the second
/// one delete the first one's controllers. A controller this junction produces
/// overwrites the same-named one IN PLACE — keeping its position, and carrying
/// over its `delay`, `reference` and `preserved`, which the decomposition does
/// not produce and must not drop. New ones are appended in `@name` order.
///
/// The replaced controller's PHASES are wholly regenerated, so foreign content
/// preserved on an old phase does not survive the cycle it belonged to; that
/// is reported through `findings()` rather than done silently.
///
/// Refuses a junction with no cycle to export — a stale id, a span junction,
/// an unsignalized one — rather than interpreting it as "remove everything".
[[nodiscard]] RM_API std::unique_ptr<Command>
sync_traffic_signals(const Scenario& scenario, const RoadNetwork& network, JunctionId junction);

/// Sets `<RoadNetwork><LogicFile @filepath>` — the `.xodr` this scenario plays
/// on. Relative paths resolve against the `.xosc`'s own directory, which is how
/// a simulator resolves them. Refuses an empty filepath: a `<LogicFile>` that
/// points nowhere is worse than none at all
/// (`asam.net:xosc:1.0.0:reference_control.road_network_availability`).
[[nodiscard]] RM_API std::unique_ptr<Command> set_logic_file(const Scenario& scenario,
                                                             std::string filepath);

/// Appends a `<ScenarioObject>` to `<Entities>`. Refuses an empty or duplicate
/// `@name` — it is the key every `entityRef` resolves through
/// (`asam.net:xosc:1.0.0:naming.unique_element_names_on_same_level`).
[[nodiscard]] RM_API std::unique_ptr<Command> add_scenario_object(const Scenario& scenario,
                                                                  ScenarioObject object);

/// Removes the `<ScenarioObject>` named `name`, and every `<Private>` in
/// `<Init>` that referenced it — leaving those behind would produce a dangling
/// `entityRef`, which `write_xosc` refuses, so the document would become
/// unwritable by a removal that appeared to succeed. Refuses a name no entity
/// carries.
[[nodiscard]] RM_API std::unique_ptr<Command> remove_scenario_object(const Scenario& scenario,
                                                                     std::string_view name);

/// Places `entity_name` at `position` via its `<Init>` `<TeleportAction>`,
/// creating the entity's `<Private>` and the action if it has none. Refuses an
/// entity name no `<ScenarioObject>` carries.
///
/// Reverts by restoring the whole `<Init><Actions>` subtree it snapshotted.
/// That is deliberately coarser than the mutation: the edit can create a
/// `Private`, create a `PrivateAction`, or overwrite a `WorldPosition`, and
/// one exact value snapshot is one code path where undoing each separately is
/// three. A scenario's init block is small, and exactness is what the
/// byte-identity contract is measured on.
[[nodiscard]] RM_API std::unique_ptr<Command> set_entity_init_position(const Scenario& scenario,
                                                                       std::string_view entity_name,
                                                                       WorldPosition position);

/// The general form of the above: places `entity_name` at any modeled
/// `Position` — world, road-relative or lane-relative (p8-s2, issue #246).
///
/// `set_entity_init_position` is the world-position overload kept for source
/// compatibility and for callers that genuinely mean a world pose; it forwards
/// here. Everything the editor authors goes through this one, with a
/// `LanePosition`.
///
/// ★ RETYPING A POSITION DROPS THE OLD ELEMENT'S PRESERVED TIER, and reports it
/// through `findings()`. A `<WorldPosition>`'s foreign attributes name a
/// different element and cannot be carried onto a `<LanePosition>`; re-emitting
/// them there would invent content the file never had.
///
/// Refuses a road-relative position that names no road, a lane position that
/// names no lane, or a negative `s` — the placement-time half of what
/// `validate_scenario` would otherwise only report at save time.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_entity_init_pose(const Scenario& scenario, std::string_view entity_name, Position position);

/// Adds a `<ScenarioObject>` AND places it, as ONE undoable command (p8-s2,
/// issue #246) — what the Actor tool pushes for a single click.
///
/// Not `add_scenario_object` followed by `set_entity_init_pose`: placing an
/// actor is one gesture and must be one undo entry, and it must be one KERNEL
/// command rather than a `QUndoCommand` with two children, because GW-6's
/// evidence is a headless Python replay and two calls do not replay as one.
///
/// Refuses an empty or duplicate `@name` and an invalid position, exactly as
/// its two halves would.
[[nodiscard]] RM_API std::unique_ptr<Command>
place_scenario_object(const Scenario& scenario, ScenarioObject object, Position position);

/// Sets `entity_name`'s initial speed [m/s] via its `<Init>`
/// `<LongitudinalAction><SpeedAction><AbsoluteTargetSpeed>`, creating the
/// entity's `<Private>` and the action if it has none.
///
/// APPENDS A SECOND `<PrivateAction>` beside any teleport rather than sharing
/// one: `<PrivateAction>` is a per-element choice, so this builds the model the
/// reader would have produced from the same file.
///
/// Refuses a negative speed (never clamps it — a clamp hides the slip), and
/// refuses an entity whose speed comes from a `<RelativeTargetSpeed>` this
/// version does not model, because `<SpeedActionTarget>` is a 1..1 union and
/// the alternatives are to emit an invalid file or to silently drop preserved
/// content.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_entity_init_speed(const Scenario& scenario, std::string_view entity_name, double speed);

/// Renames a `<ScenarioObject>` AND every `entityRef` that resolved through it.
///
/// ★ THE REFERENCES ARE THE POINT. `@name` is the key `<Private>` resolves
/// through, so renaming the entity alone leaves a dangling `entityRef` that
/// `write_xosc` refuses — a rename that appeared to succeed would make the
/// document unsavable. Refuses an empty or already-taken new name, and a name
/// no entity carries.
[[nodiscard]] RM_API std::unique_ptr<Command>
rename_scenario_object(const Scenario& scenario, std::string_view from, std::string to);

// --- routes (p8-s3, issue #247) ----------------------------------------------
//
// A route is authored ONE waypoint at a time, so the factories are the ones a
// tool needs for a click, a drag and a Delete — not one all-at-once setter. The
// coarse alternative (replace the whole route on every gesture) would make a
// single dragged waypoint an undo entry that also silently re-writes the ones
// the user did not touch.

/// Assigns `route` to `entity_name`, replacing any route it already has.
///
/// Creates the entity's `<Private>` and its `<RoutingAction>` if it has none;
/// the routing action is APPENDED as its own `<PrivateAction>`, the same shape
/// `set_entity_init_speed` uses, because `<PrivateAction>` is a per-element
/// choice and that is the model the reader would have produced from the file.
///
/// Refuses an entity no `<ScenarioObject>` carries, an empty route name, and a
/// route with fewer than two waypoints — the schema's own minimum, brought
/// forward from save time so a tool cannot build a document it then cannot
/// write.
[[nodiscard]] RM_API std::unique_ptr<Command>
assign_route(const Scenario& scenario, std::string_view entity_name, Route route);

/// Removes `entity_name`'s `<RoutingAction>` entirely.
///
/// The whole action rather than its waypoints: a route with no waypoints is not
/// a shorter route, it is an invalid element. Refuses an entity with no route.
[[nodiscard]] RM_API std::unique_ptr<Command> clear_route(const Scenario& scenario,
                                                          std::string_view entity_name);

/// Moves `entity_name`'s route waypoint at `index` to `position`.
///
/// The drag commit. Carries the waypoint's `@routeStrategy` and its preserved
/// tier through untouched — neither belongs to the position being moved.
///
/// Refuses an entity with no route and an index the route does not have; never
/// clamps, because a clamped index writes a waypoint the caller did not name.
[[nodiscard]] RM_API std::unique_ptr<Command> set_route_waypoint(const Scenario& scenario,
                                                                 std::string_view entity_name,
                                                                 std::size_t index,
                                                                 Position position);

/// Inserts a waypoint at `index` (which may be the route's size, to append).
///
/// Refuses an entity with no route and an index past the end.
[[nodiscard]] RM_API std::unique_ptr<Command> insert_route_waypoint(const Scenario& scenario,
                                                                    std::string_view entity_name,
                                                                    std::size_t index,
                                                                    RouteWaypoint waypoint);

/// Removes the waypoint at `index`.
///
/// ★ REFUSES WHEN IT WOULD LEAVE FEWER THAN TWO. "At least two waypoints are
/// needed to define a route", so removing the second-to-last one would produce
/// a document `write_xosc` refuses — a deletion that appeared to succeed and
/// then made the scenario unsavable. Delete the whole route instead
/// (`clear_route`), which is what the user meant.
[[nodiscard]] RM_API std::unique_ptr<Command>
remove_route_waypoint(const Scenario& scenario, std::string_view entity_name, std::size_t index);

// --- the storyboard (p8-s4, issue #248) --------------------------------------
//
// ★ TWO FACTORIES, EACH TAKING A WHOLE `<Story>` — deliberately coarser than
// the gestures the editor makes on it, which is the call
// `set_entity_init_position` already documents: "one exact value snapshot is
// one code path where undoing each separately is" a dozen. A storyboard is a
// six-level tree (Story ▸ Act ▸ ManeuverGroup ▸ Maneuver ▸ Event ▸ Action), so
// a per-node API would be thirty factories carrying a five-deep index path, and
// every one of them a place for the byte-identity contract to break.
//
// The panel builds the modified `Story` VALUE and commits one command per
// gesture; the undo entry is exact because the snapshot is the story it
// replaced. A `Story` is also plain data with no handles in it, so a Python
// caller builds one and replays the same command headlessly — which is what
// GW-6's evidence is.

/// Replaces `<Storyboard><Story>` at `index`, or APPENDS when `index` equals
/// the story count.
///
/// Refuses an empty or duplicate `@name` (`<Story>` names are unique among
/// siblings, and a `StoryboardElementStateCondition` resolves through one), a
/// story with no `<Act>`, and an index past the end — never clamps, because a
/// clamped index rewrites a story the caller did not name.
///
/// ★ DOES NOT VALIDATE THE STORY'S CONTENTS BEYOND THAT. The deeper checks —
/// dangling `entityRef`s, a `@phase` that names no synthesized phase, an event
/// with no action — belong to `validate_scenario`, which sees the whole
/// document; a factory that duplicated them would be a second implementation
/// drifting from the first. Push, then read `validate_scenario`.
[[nodiscard]] RM_API std::unique_ptr<Command>
set_story(const Scenario& scenario, std::size_t index, Story story);

/// Removes the `<Story>` at `index`. Refuses an index the storyboard does not
/// have.
[[nodiscard]] RM_API std::unique_ptr<Command> remove_story(const Scenario& scenario,
                                                           std::size_t index);

/// Replaces `<Storyboard><StopTrigger>` — what ends the scenario.
///
/// ALWAYS PRESENT in the model (`Storyboard::stop_trigger` is a value, not an
/// optional), so this is a replace and never an add/remove pair; an empty
/// trigger is the legal "never stops" state the schema skeleton emits.
[[nodiscard]] RM_API std::unique_ptr<Command> set_stop_trigger(const Scenario& scenario,
                                                               Trigger trigger);

/// Replaces `entity_name`'s `<BoundingBox>`, carrying over the element's own
/// preserved tier (which belongs to the element, not to the numbers).
///
/// Refuses a non-positive dimension, and an entity that is neither a
/// `<Vehicle>` nor a `<Pedestrian>` — a `std::monostate` entity object is a
/// catalog reference or a `MiscObject` riding the preserved tier, and it has no
/// bounding box this version can reach.
[[nodiscard]] RM_API std::unique_ptr<Command> set_scenario_object_bounding_box(
    const Scenario& scenario, std::string_view entity_name, BoundingBox box);

} // namespace roadmaker::osc::edit
